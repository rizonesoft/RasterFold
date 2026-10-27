---
schema_version: 1
id: model-experiments
domain: 01-compression
status: draft
title: "TODO-02 -- Model Experiments for Method 1"
depends_on: []
track: C2
---

# TODO-02 -- Model Experiments for Method 1

> **Goal:** Each promising change to method 1's modelling is measured on the research corpus against a stated bar, and ends as either an accepted variant (a patch committed under `research/variants/` for the format version 3 port) or a negative result recorded in the "Already tried" table of `docs/research.md`. No section here changes `src/`: the port happens once, for all accepted variants together, in `D01 T03 §1`.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** Method 1 mixes five context models (`NMODEL 5`) with one weight set selected by `wsel`, adaptive-rate counters with one visit count per context row (`RATE[n]`, `n` capped at 60), and initial counter probability 1/2. Baseline, measured 2026-09-29 with `python research/ab.py size <set>` on `src/cm.c`: `photo` total 0.9712 of JPEG XL e7, geometric mean 0.9728, worst kodim17 1.0037; `tp` total 0.9743, worst 0.9931; `synth` geometric mean 1.1986. Speed: about 440 thread cycles per sample on one thread (`research/bench.c`). The designs are `docs/research.md` sections 4.2 to 4.7; section 3 lists what was already tried.
>
> **Measured 2026-10-27 (`D01 T02 §1`, totals/geomean/worst per set):** baseline `photo` 30865211 / 0.9728 / 1.0037 (kodim17), `tp` 6651583 / 0.9722 / 0.9931 (tp_nightshot_iso_1600_crop), `synth` 206566 / 0.5883 / 1.5665 (syn_gradient). 7-model: `photo` 30812732 / 0.9698 / 1.0000 (kodim17), `tp` 6641390 / 0.9700 / 0.9928 (tp_nightshot_iso_1600_crop), `synth` 206229 / 0.5760 / 1.4647 (syn_gradient). k5-only: `photo` 30836880 / 0.9711 / 1.0016 (kodim17), `tp` 6645737 / 0.9710 / 0.9926 (tp_nightshot_iso_1600_crop), `synth` 206344 / 0.5810 / 1.4972 (syn_gradient). k6-only: `photo` 30826654 / 0.9708 / 1.0012 (kodim17), `tp` 6643903 / 0.9706 / 0.9931 (tp_nightshot_iso_1600_crop), `synth` 206317 / 0.5790 / 1.4924 (syn_gradient). Timing: `ab -n5` B/A enc 1.239 / dec 1.217 (-t1), enc 1.260 / dec 1.267 (-t3); `vsjxl.py -n5` scratch-CLI decode min 0.578/0.571 (both 3-thread), 0.400 (JXL 1-thread).
<!-- claim: count "^#define NMODEL 5$" src/cm.c = 1 -->
<!-- claim: count "^#define NODES 32 " src/cm.c = 1 -->
<!-- claim: count "^#define WSTRIDE 8 " src/cm.c = 1 -->
<!-- claim: exists research/variants/models-6-7.patch -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- sections 2.3 and 2.4 (the measurement protocol), 3 (already tried), and 4.2 to 4.7 (the designs these sections test)
- [`src/cm.c`](../../src/cm.c) -- `cm_model` (contexts), `code_bit` (mixer and counters), `cm_alloc` (initial state)
- [`research/ab.py`](../../research/ab.py) -- `size`, `ab`, and `bench`, which measure a `cm.c` variant
- -> XREF: D01 T03 §1 -- ports every variant these sections accept, as format version 3
- -> XREF: D01 T05 §2 -- builds the encoder search when §6's ceiling clears 0.3%

## Outcome

- Every section ends with its measurement quoted against its bar and a row in `docs/research.md`'s "Already tried" table (accepted or rejected).
- Every accepted change exists as `research/variants/<name>.patch`, a diff against `src/cm.c` that `git apply --check` accepts at the commit that adds it.

**Adjacency:** all=not-applicable (research measurements; nothing here changes what the tool reads, writes, or reports)

**Adjacency rationale:** These sections decide what a later format version contains. The format duties (oracle, spec, known-answer file, freeze check) belong to the port in `D01 T03 §1`.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | A sixth and seventh context model             | --         |  [x]   |
|   2   |   §2    | Two adaptation speeds per context             | --         |  [x]   |
|   3   |   §3    | A second mixer weight set                     | --         |  [x]   |
|   4   |   §4    | Learning cost on small images                 | --         |  [ ]   |
|   5   |   §5    | An exact-repeat context                       | --         |  [ ]   |
|   6   |   §6    | The ceiling of encoder-chosen parameters      | --         |  [ ]   |
|   7   |   §7    | The accepted variants measured together       | §1, §2, §3, §4, §5, §6 |  [ ]   |

---

## 1. A Sixth and Seventh Context Model

> **Started:** 2026-10-27T16:23:58Z

**Needs:** Idle machine (timing)

In the floating-point research harness two more neighbour contexts took the `sub` total from 8,246,827 to 8,228,829 bytes (-0.22%) and improved the worst image; the integer codec has never tried them. Bar: at least 0.1% on the `photo` total with the worst image no more than 0.1% worse, confirmed on `tp`, and three-thread decode still below 0.6 of JPEG XL's on `measure/image.bmp`; otherwise the negative result ships.

**Corrected 2026-10-27:** Per the pre-build `--kind plan` review (`docs/reviews/01-compression/D01-T02-s1.md`): item 1 now wires the `kk` initializer (raising `NMODEL` alone pins the new models to counter row zero while round trips still pass); item 2 names the six-model `kk` wirings; item 3 builds a scratch Windows CLI from the winning variant and measures three-thread decode against JPEG XL directly with `research/vsjxl.py` (the `ab` B/A ratio alone cannot prove the 0.6 bar); item 4 generates the patch with `diff --label` headers targeting `src/cm.c` on both sides (plain `git diff --no-index` emits mismatched old/new paths).

- [x] In `build/research/try.c` (a copy of `src/cm.c`), set `NMODEL` to 7, add `WW` to the neighbour block of `cm_model` (`P(x-2, y)` when `x > 1`, else `W`), compute `k5 = (q9(NE - pred) * 9 + q9(NW - pred)) * 16 + qE` and `k6 = ((qn7(WW - pred) * 7 + qn7(NN - pred)) * 7 + qn7(NNE - pred)) * 16 + qE` (`qn7` is `k0`'s scaled 7-level quantizer), extend the `kk` initializer to `{k0, k1, k2, k3, k4, k5, k6}` (`src/cm.c:463`; without it the new entries are zero and both models read counter row zero), and extend `code_bit` with the two inputs and the bias moved to weight slot 7. Done when: `python research/ab.py size build/research/try.c sub` decodes every image. Cheaper substitute: measuring on `measure` alone.
- [x] Measure `python research/ab.py size build/research/try.c photo`, `tp`, and `synth`, and the six-model variants with only `k5` (`NMODEL` 6, `kk` `{k0, k1, k2, k3, k4, k5}`, bias slot 6) and only `k6` (`kk` `{k0, k1, k2, k3, k4, k6}`, bias slot 6). Done when: all three variants' summary lines are quoted beside the baseline in the Current state block.
- [x] Measure decode speed with `python research/ab.py ab src/cm.c build/research/try.c -n5 -t1 measure kodim01` and `-t3`. Then build a scratch Windows CLI from the winning variant (compile stems `platform/api/crc32/bmp/fold/cm/pal/mem/sys/main` from `src/` with `build/research/try.c` in place of `src/cm.c`, following the per-stem flags and `rf_start` link in `research/ab.py` compat) and run `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3 --exe <scratch>` on `measure/image.bmp` (Windows ffmpeg with libjxl, proven by the Milestone 3 gate runs on 2026-10-26). Done when: both minimum-time ratios are quoted, and the scratch CLI's three-thread decode ratio against JPEG XL is quoted against the 0.6 bar.
- [x] Record the best variant's result in `docs/research.md` section 3; if it clears the bar, save it as `research/variants/models-6-7.patch` with `diff -u --label a/src/cm.c --label b/src/cm.c src/cm.c build/research/try.c > research/variants/models-6-7.patch` (`diff` exits 1 when the files differ, the expected case). Done when: the table row exists, and the patch exists exactly when the bar was cleared.
- [x] Commit: `"research: measure a sixth and seventh context model"`

**Test checkpoint:** the quoted `ab.py size` and `ab` lines against the baseline and the quoted `vsjxl.py` three-thread decode ratio against JPEG XL against the 0.6 bar; if a patch was saved, `git apply --check research/variants/models-6-7.patch` exits 0.

> **Verified:** 2026-10-27 | §1 | 03d278868985ea92edd8a2f80f2d448e60a65ff2
> **Review:** `docs/reviews/d01-t02-s1.json`
## 2. Two Adaptation Speeds per Context

> **Started:** 2026-10-27T17:28:14Z

**Needs:** Idle machine (timing)

The counters slow to 1/61.5 once a row has been visited 60 times. A second, fast counter per node (fixed rate 1/16) reacts to local change, and the mixer learns when to trust each (the two-rate state of lpaq and paq8). Bar: as §1, and decode no more than 10% slower per 0.1% gained.

**Acceptance:** let `g` be the winner's `photo` total gain in percent. The permitted `-t3` minimum-time `ab` decode ratio is `1 + (g / 0.1) * 0.10` (a 0.1% gain permits 1.10, a 0.2% gain 1.20). The winner is accepted only if its `-t3` minimum-time decode ratio is at most that AND its JPEG XL minimum-time decode ratio (`rf_dec` min / `jxl_dec` min from the `vsjxl.py` lines, not the printed median) is below 0.6. The `-t1` ratios are recorded alongside but do not govern.

**Corrected 2026-10-27 (completion review `docs/reviews/01-compression/D01-T02-s2.md`):** the Acceptance formula was written `1 + 10 * (g / 0.1)`, which gives 11.0 for a 0.1% gain and contradicts its own examples; it now reads `1 + (g / 0.1) * 0.10`, matching the examples and the Bar above (10% slower per 0.1% gained). The findings file's worked figure for the best stage (0.92 at g = -0.008%) is corrected to 0.992 on the same basis. The verdict is unchanged under either reading, because no stage gained on photo at all.

**Corrected 2026-10-27:** "Bar: as §1" imports §1's JPEG XL clause (three-thread decode below 0.6), but no item measured it (same gap class as the `D01 T02 §1` plan P1); item 2 now times `-t3` and runs the scratch-CLI `vsjxl.py` comparison for the winner. The stages now keep per-stage files so the winner's patch is generatable (evolving one `try.c` would lose the earlier stages), and item 3 uses the `diff --label` patch recipe (per the §1 plan P2).

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T02-s2.md`):** item 1 now names the per-stage input-slot map, initial weights, dot product, bias, and update coverage (the 8-wide SIMD update cannot train `WSTRIDE` 16 alone), plus a participation check per stage; the **Acceptance** calculation names the governing ratios; item 3 and the checkpoint spell out the negative branch (no winner, no patch, no `vsjxl.py` run).

- [x] In three copies of `src/cm.c` (`build/research/try-f0.c`, `try-f01.c`, `try-fall.c`), double `NODES` to 64 with slot `node + 32` the fast counter (starting at 32768, updated by `c += ((target - c) * 4096) >> 16`), keeping `NMODEL` at 5 throughout. Wire the mixer per stage: `try-f0.c` inputs 0-4 slow plus model 0 fast at 5, bias 6, init slots 0-5 to 19661 (existing 8-wide update covers 0-7); `try-f01.c` adds model 1 fast at 6, bias 7, init 0-6; `try-fall.c` adds all five fast at 5-9, bias 11, `WSTRIDE` 16, init 0-9, dot over 10 terms plus `w[11] * 64`, and scalar update tail for slots 8-11 (the SIMD pair covers 0-7 only). Verify participation per stage with a temporary weight-drift print on one image (fast weights must leave 19661), removed before measuring. Done when: `python research/ab.py size <each file> sub` decodes every image and drift was observed on all three. (Done 2026-10-27: built; `size <stage> sub` decodes every image (f0 photo-total line queued), participation prints observed then removed: f0 node32 w5 19661 -> 17227; f01 w5 -> 16540, w6 -> 8444; fall w5 -> 14092, w9 -> 6067, w11 0 -> 3076.)
- [x] Measure `photo`, `tp`, and `synth` on all three stage files. Time each variant with `python research/ab.py ab src/cm.c <variant> -n5 -t1 measure kodim01` and `-t3`. If a winner emerges (largest `photo` gain clearing the size bar), build a scratch Windows CLI from its file (same recipe as §1: compile stems `platform/api/crc32/bmp/fold/cm/pal/mem/sys/main` from `src/` with the variant in place of `src/cm.c`, following the per-stem flags and `rf_start` link in `research/ab.py` compat) and run `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3 --exe <scratch>` on `measure/image.bmp`; then evaluate the Acceptance calculation for it. Done when: each variant's full summary lines and `-t1`/`-t3` ratios are quoted, the Acceptance verdict is shown, and (winner only) its three-thread decode ratio against JPEG XL is quoted against the 0.6 bar. (Done 2026-10-27: full lines quoted in `docs/reviews/01-compression/D01-T02-s2.md` Measurements. No stage gains on photo: f0 +0.025%, f01 +0.008%, fall +0.186% vs baseline 30,865,211; tp and synth likewise flat or worse. Decode B/A: f0 1.054 -t1 / 1.078 -t3, f01 1.103 / 1.352, fall 1.214 / 1.340. Best gain g = -0.008% makes the permitted ratio 0.992 (corrected formula below), unreachable: no winner, no scratch CLI, no `vsjxl.py` run.)
- [x] Record the result in `docs/research.md` section 3: the winner's row plus patch, or all three stages' rejection numbers with no patch and no `vsjxl.py` run. A winner is saved as `research/variants/fast-counters.patch` with `diff -u --label a/src/cm.c --label b/src/cm.c src/cm.c <winner file> > research/variants/fast-counters.patch` (`diff` exits 1 when the files differ, the expected case). Done when: the table row exists, and the patch exists exactly when the bar was cleared. (Done 2026-10-27: rejection row added in section 3.3 with all three stages' numbers; `research/variants/fast-counters.patch` absent, as the bar was not cleared.)
- [x] Commit: `"research: measure two adaptation speeds per context"`

**Test checkpoint:** the quoted `ab.py size` and `ab` lines for all three stages against the baseline; if a winner cleared the bar, its quoted `vsjxl.py` three-thread minimum-time decode ratio against the 0.6 bar and `git apply --check research/variants/fast-counters.patch` exiting 0, else the quoted rejection record and no patch file.

> **Verified:** 2026-10-27 | §2 | 05fa6ae7b159fe9511be9a313832c0a0261e27d9
> **Review:** `docs/reviews/d01-t02-s2.json`
## 3. A Second Mixer Weight Set

> **Started:** 2026-10-27T18:12:40Z

**Needs:** Idle machine (timing)

Mixer weights depend only on `wsel` (the error energy and the G residual class). Averaging two mixers whose weight sets are chosen by different contexts is a standard gain in paq-style coders. Bar: as §2.

**Acceptance:** let `g` be the variant's `photo` total gain in percent. The permitted `-t3` minimum-time `ab` decode ratio is `1 + (g / 0.1) * 0.10` (a 0.1% gain permits 1.10, a 0.2% gain 1.20). The variant is accepted only if its `-t3` minimum-time decode ratio is at most that AND its JPEG XL minimum-time decode ratio (`rf_dec` min / `jxl_dec` min from the `vsjxl.py` lines, not the printed median) is below 0.6. The `-t1` ratios are recorded alongside but do not govern.

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T02-s3.md`):** the rejection branch now distinguishes size rejection (no `vsjxl.py` run) from timing rejection (the `vsjxl.py` figures are reported); both leave no patch.

**Corrected 2026-10-27:** §3 as written repeats the gap classes the §1/§2 reviews caught, so before building: the variant is built in a fresh copy `build/research/try-m2.c` of `src/cm.c` (`build/research/try.c` still holds §1's 7-model winner; building there would change the baseline and make the winner's patch ungeneratable). Item 1 names the full wiring: `chan_t` gains `wt2` (a second table the size of `wt`, `WSEL` 128 rows, so `wsel2 = k2 % 128` always lands in range), allocated and initialised in `cm_alloc` exactly like `wt` (slots 0-4 to 19661, the rest 0); `stage_t` gains `wrow2`, set in `cm_model` beside `wrow` from the `k2` already computed there; `code_bit` takes the second row, computes the second dot product over the same five stretched counters plus the bias slot, averages `x = (x1 + x2) >> 1` from the two `>> 16` dots, clamps once, and applies the same SIMD weight update to both rows; plus a participation check per the §2 pattern (temporary weight-drift print, removed before measuring). Item 2 names the sets, timing commands, the winner-only scratch CLI and `vsjxl.py` run, and the Acceptance evaluation; item 3 and the checkpoint spell out the negative branch (no winner, no patch, no `vsjxl.py` run) and the `diff --label` patch recipe.

- [x] In a fresh copy of `src/cm.c` at `build/research/try-m2.c`, add a second weight table `wt2` the size of `wt` (allocated and initialised in `cm_alloc` exactly like `wt`), select its row by `wsel2 = k2 % 128` (set `stage_t.wrow2` in `cm_model` beside `wrow`), compute a second dot product in `code_bit` over the same stretched counters and bias slot, use `x = (x1 + x2) >> 1` from the two `>> 16` dots before clamping and `SQUASH`, and update both rows with the same error. Verify participation with a temporary weight-drift print on one image (the `wt2` weights must leave their initial values), removed before measuring. Done when: `python research/ab.py size build/research/try-m2.c sub` decodes every image and drift was observed. (Done 2026-10-27: built in `build/research/try-m2.c`; `size ... sub` decodes every image; participation print observed then removed: node 5 w2_0 19661 -> 19907, w2_5 0 -> -160 on kodim01.)
- [x] Measure `photo`, `tp`, and `synth` on `build/research/try-m2.c`. Time it with `python research/ab.py ab src/cm.c build/research/try-m2.c -n5 -t1 measure kodim01` and `-t3`. If it clears the size bar, build a scratch Windows CLI from its file (compile stems `platform/api/crc32/bmp/fold/cm/pal/mem/sys/main` from `src/` with the variant in place of `src/cm.c`, following the per-stem flags and `rf_start` link in `research/ab.py` compat) and run `python3 scripts/windows.py research/vsjxl.py -n5 --threads 3 --exe <scratch>` on `measure/image.bmp`; then evaluate the Acceptance calculation. Done when: the variant's full summary lines and `-t1`/`-t3` ratios are quoted, the Acceptance verdict is shown, and (winner only) its three-thread decode ratio against JPEG XL is quoted against the 0.6 bar. (Done 2026-10-27: full lines in `docs/reviews/01-compression/D01-T02-s3.md` Measurements. Photo +0.030% vs baseline, tp +0.036%, synth -0.052%; decode B/A 1.130 -t1 / 1.099 -t3. g = -0.030% makes the permitted ratio 0.970: size rejection, no scratch CLI, no `vsjxl.py` run.)
- [x] Record the result in `docs/research.md` section 3: the winner's row plus patch, or the rejection numbers with no patch (a size rejection runs no `vsjxl.py`; a timing rejection reports the `vsjxl.py` figures it measured). A winner is saved as `research/variants/second-mixer.patch` with `diff -u --label a/src/cm.c --label b/src/cm.c src/cm.c build/research/try-m2.c > research/variants/second-mixer.patch` (`diff` exits 1 when the files differ, the expected case) and verified with `git apply --check`. Done when: the table row exists, and the patch exists exactly when the bar was cleared. (Done 2026-10-27: rejection row added in section 3.3; `research/variants/second-mixer.patch` absent, as the bar was not cleared.)
- [x] Commit: `"research: measure a second mixer weight set"`

**Test checkpoint:** the quoted `ab.py size` and `ab` lines for the variant against the baseline; if the bar was cleared, its quoted `vsjxl.py` three-thread minimum-time decode ratio against the 0.6 bar and `git apply --check research/variants/second-mixer.patch` exiting 0, else the quoted rejection record and no patch file (a timing rejection quotes the `vsjxl.py` minimum-time ratio it failed).

> **Verified:** 2026-10-27 | §3 | c492fd51e6db224f539c9b0e1aacfd440ddf98cf
> **Review:** `docs/reviews/d01-t02-s3.json`
## 4. Learning Cost on Small Images

kodim17 (+0.37%) and kodim09 (+0.19%), both 768x512, are the only photographs still larger than JPEG XL; small images spend a larger share of their samples learning from counters that start at 1/2. Bar: at least 0.1% on the `kodak` total with no image of `photo` worse by more than 0.05%.

- [ ] In `build/research/try.c`, initialize model 1's zero-flag counter (node 1) from its row's energy `qE = k1 / 81` as `p = 65535 - 4096 * qE` clamped to `[8192, 57344]`. This needs `cm_alloc` to know each row's `qE`, which for model 1 is the row index divided by 81. Done when: `python research/ab.py size build/research/try.c kodak` decodes every image. Cheaper substitute: a constant tuned on kodim17.
- [ ] Measure `kodak`, `photo`, and `tp`; then, separately, start model 0's mixer weight at 29491 (0.45) and the others at 16384 (0.25), and measure again. Done when: both variants' summary lines are quoted.
- [ ] Record the result in `docs/research.md` section 3, and save a winner as `research/variants/small-image-start.patch`. Done when: the table row exists, and the patch exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure the learning cost on small images"`

**Test checkpoint:** the quoted `ab.py size kodak` and `photo` lines against the baseline, with kodim17's and kodim09's ratios.

## 5. An Exact-Repeat Context

Anti-aliased screenshots and diagrams with more than 256 colours still contain long exact runs, which a context of neighbour equalities captures and method 2 cannot reach. Bar: a clear gain on `synth` (geometric mean at least 2% lower) with the `photo` total no more than 0.05% worse.

- [ ] In `build/research/try.c`, add a model with context `eq * 16 + qE`, where `eq = (W==N) | (N==NE)<<1 | (W==NW)<<2 | (NW==N)<<3 | (W==WW)<<4 | (N==NN)<<5` over this channel's plane values (`WW` as in §1). Done when: `python research/ab.py size build/research/try.c synth -v` decodes every image.
- [ ] Measure it as a sixth model, and as a replacement for each existing model in turn (the one whose removal costs least), on `synth`, `photo`, and `tp`. Done when: every variant's summary line is quoted.
- [ ] Record the result in `docs/research.md` section 3, and save a winner as `research/variants/repeat-context.patch`. Done when: the table row exists, and the patch exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure an exact-repeat context"`

**Test checkpoint:** the quoted `ab.py size synth` and `photo` lines against the baseline.

## 6. The Ceiling of Encoder-Chosen Parameters

An encoder that tries a few parameter sets per channel and signals the best in the header keeps the decoder's cost unchanged. Before building it, measure its ceiling: the sum over images of the best fixed candidate per image. Bar: a ceiling at least 0.3% below the `photo` total; otherwise the idea is recorded as not worth a header byte.

- [ ] Build one `cm.c` variant per candidate in `build/research/`: LMS step 6, 10 (today), and 16; WP sub-predictor weight maxima `{13, 12, 12, 12}` (today) and `{16, 10, 10, 10}`. Done when: `python research/ab.py size <variant> photo -v` runs for each.
- [ ] Compute the per-image minimum over the candidates from the `-v` outputs with a short script under `research/` (`research/ceiling.py`), and its total against JPEG XL. Done when: the script prints the ceiling total and the per-image winner counts.
- [ ] Record the ceiling in `docs/research.md` section 3 (with the candidate list) and, if it clears the bar, file the encoder search and its header byte through `add-todo` as a section of this file. Done when: the table row exists, and the follow-up section exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure the ceiling of encoder-chosen parameters"`

**Test checkpoint:** `python research/ceiling.py` output quoted beside the baseline total.

## 7. The Accepted Variants Measured Together

**Needs:** Idle machine (timing)

Gains measured one at a time do not simply add: two models can capture the same redundancy. Before the port, the accepted variants of §1 to §6 are combined and measured once, so `D01 T03 §1` ports a measured whole.

- [ ] Apply every patch under `research/variants/` to a fresh copy of `src/cm.c` in `build/research/combined.c`, resolving overlaps. Done when: `python research/ab.py size build/research/combined.c sub` decodes every image. If no variant was accepted, record that here, strike the remaining items with that reason, and stop.
- [ ] Measure `photo`, `tp`, `synth`, and `-t1`/`-t3` decode ratios, and drop any variant whose removal costs less than 0.05% on `photo`. Done when: the combined summary lines and the drop decisions are quoted.
- [ ] Save the combination as `research/variants/version-3.patch` and list its parts in `D01 T03 §1`'s context paragraph. Done when: `git apply --check research/variants/version-3.patch` exits 0.
- [ ] Commit: `"research: measure the accepted variants together for format version 3"`

**Test checkpoint:** the combined `ab.py size photo` line against the 2026-09-29 baseline (0.9712), and `git apply --check` output.

## Verification

- [ ] Every section's row in `docs/research.md` section 3 exists and matches its stamp
- [ ] `python scripts/todo-graph.py validate` clean
