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
|   4   |   §4    | Learning cost on small images                 | --         |  [x]   |
|   5   |   §5    | An exact-repeat context                       | --         |  [x]   |
|   6   |   §6    | The ceiling of encoder-chosen parameters      | --         |  [x]   |
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

> **Started:** 2026-10-27T18:30:34Z

kodim17 (+0.37%) and kodim09 (+0.19%), both 768x512, are the only photographs still larger than JPEG XL; small images spend a larger share of their samples learning from counters that start at 1/2. Bar: at least 0.1% on the `kodak` total with no image of `photo` worse by more than 0.05%.

**Needs:** Idle machine (timing)

**Acceptance:** a variant clears the bar when its `kodak` total gain against the baseline is at least 0.1%, no `photo` image is more than 0.05% worse than the baseline (per-image sizes from `ab.py size -v`), AND its `tp` total is no worse than the baseline (a variant that improves `photo` while `tp` gets worse is overfitted, per the measurement protocol). If both variants clear, the winner is the one with the larger `kodak` gain. Both variants change initialisation only; the per-sample decode work is unchanged, but `cm_alloc` runs on the decode path, so decode timing is measured and recorded (no fixed slowdown bar: the initialisation cost is expected to be negligible, and the recorded ratios prove or refute that). If neither clears, there is no winner and no patch.

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T02-s4.md`):** the Acceptance now includes the protocol's held-out condition (`tp` total no worse than baseline, else the variant is overfitted and rejected); item 2 also measures `synth` for the baseline and both variants (the protocol's reporting set); and because `cm_alloc` runs on the decode path (`src/cm.c` decode entry), the 'speed unchanged by construction' exemption is replaced by measured decode timing, so the section carries `Needs: Idle machine (timing)`.

**Corrected 2026-10-27:** §4 as written repeats the gap classes the §1-§3 reviews caught, so before building: the two variants are built as separate fresh copies of `src/cm.c` (`build/research/try-s1.c` and `build/research/try-s2.c`), each measured against the same baseline (`build/research/try.c` still holds §1's 7-model winner). Item 1 names the exact counter cell (model 1, node 1, per row) and the clamp; item 2 names the per-image evidence the Bar needs (`size -v` on `photo` for baseline and both variants, quoting kodim17 and kodim09); the **Acceptance** paragraph defines the winner when both variants clear and states why no timing clause applies; item 3 and the checkpoint spell out the negative branch and the `diff --label` patch recipe. The 'cheaper substitute' sentence in item 1 is dropped: tuning a constant on kodim17 alone would tune to one image, which the measurement rules forbid.

- [x] In a fresh copy of `src/cm.c` at `build/research/try-s1.c`, initialize model 1's zero-flag counter (node 1) from its row's energy: in `cm_alloc`, after the flat counter init, for each row `r` of model 1 set the node-1 counter to `65535 - 4096 * (r / 81)` clamped to `[8192, 57344]` (model 1's row index is `k1`, and `k1 / 81` is its `qE`). Done when: `python research/ab.py size build/research/try-s1.c kodak` decodes every image.
- [x] In a second fresh copy at `build/research/try-s2.c`, start model 0's mixer weight at 29491 (0.45) and the other models' at 16384 (0.25) in the `cm_alloc` weight init (slot 0 to 29491, slots 1-4 to 16384, the rest 0, all `WSEL` rows). Measure `kodak`, `photo`, `tp`, and `synth` on both variants, plus `python research/ab.py size -v <file> photo` per-image lines for the baseline and both variants. Time each variant with `python research/ab.py ab src/cm.c <variant> -n5 -t1 measure kodim01` and `-t3`, repeated until two runs agree on the decode ratio. Done when: both variants' summary lines are quoted and kodim17's and kodim09's per-image ratios against the baseline are quoted. (Done 2026-10-27: both variants measured on kodak/photo/tp/synth with per-image `photo` lines; s1 kodak -0.001%, s2 kodak +0.057%; kodim17 -0.0021% (s1) / +0.0412% (s2), kodim09 -0.0010% / +0.0354%; decode timing scatters around 1.0. Full lines in `docs/reviews/01-compression/D01-T02-s4.md` Measurements.)
- [x] Record the result in `docs/research.md` section 3, and save a winner as `research/variants/small-image-start.patch` with `diff -u --label a/src/cm.c --label b/src/cm.c src/cm.c <winner file> > research/variants/small-image-start.patch` (`diff` exits 1 when the files differ, the expected case) verified with `git apply --check`; if neither variant clears the bar, record both rejections and save no patch. Done when: the table row exists, and the patch exists exactly when the bar was cleared. (Done 2026-10-27: rejection row added in section 3.3; `research/variants/small-image-start.patch` absent, as neither variant cleared the bar.)
- [x] Commit: `"research: measure the learning cost on small images"`

**Test checkpoint:** the quoted `ab.py size kodak`, `photo`, `tp`, and `synth` lines for both variants against the baseline, with kodim17's and kodim09's per-image ratios, the worst `photo` per-image change, the `tp` verdict, and the agreed `-t1`/`-t3` decode ratios; if a winner cleared the bar, `git apply --check research/variants/small-image-start.patch` exiting 0, else the quoted rejection record and no patch file.

> **Verified:** 2026-10-27 | §4 | b82de3505a55a74c88f71b01c856373b29912be9
> **Review:** `docs/reviews/d01-t02-s4.json`
## 5. An Exact-Repeat Context

> **Started:** 2026-10-27T18:52:49Z

**Needs:** Idle machine (timing)

Anti-aliased screenshots and diagrams with more than 256 colours still contain long exact runs, which a context of neighbour equalities captures and method 2 cannot reach. Bar: a clear gain on `synth` (geometric mean at least 2% lower) with the `photo` total no more than 0.05% worse.

**Acceptance:** a variant clears the bar when its `synth` geometric mean is at least 2% below the baseline's, its `photo` total is no more than 0.05% worse than the baseline, AND its `tp` total is no worse than the baseline (the protocol's held-out condition). If several variants clear, the winner is the one with the largest `synth` geometric-mean gain. A winner's decode timing is measured and recorded; the combined speed of all accepted variants is judged in `D01 T02 §7`, so no separate slowdown bar applies here. If none clears, there is no winner and no patch.

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T02-s5.md`):** `WW` is not computed in `src/cm.c` (only `W`, `N`, `NW`, `NE`, `NN`, `NNE` are; `WW` exists in §1's variant), so every variant first adds `WW = x > 1 ? r0[x - 2] : W` beside the other neighbour values, matching §1's definition. The sixth model also changes the SIMD input vector in `code_bit` to `{s0, s1, s2, s3, s4, s5, 64, 0}` so slot 5 trains the new counter and slot 6 the bias.

**Corrected 2026-10-27:** §5 as written repeats the gap classes the §1-§4 reviews caught, so before building: the six variants are built as separate fresh copies of `src/cm.c` (`build/research/try-r6.c` for the sixth model, `build/research/try-r0.c` to `try-r4.c` for the replacements), each measured against the same baseline (`build/research/try.c` still holds §1's 7-model winner). Item 1 names the wiring: `keq = eq * 16 + qE` from the `W`, `N`, `NE`, `NW`, `WW`, `NN` plane values `cm_model` already computes (with its edge rules); the sixth model sets `NMODEL 6` and extends `code_bit` by one counter exactly as §1's variant extended it (sixth stretched counter in the dot, bias moving to slot 6, the existing 8-wide update covering slots 0-7); each replacement swaps one `kk[j]` for `keq` and changes nothing else. The **Acceptance** paragraph adds the protocol's `tp` condition and names the winner rule; item 3 and the checkpoint spell out the negative branch and the `diff --label` patch recipe.

- [x] In a fresh copy of `src/cm.c` at `build/research/try-r6.c`, add the exact-repeat context as a sixth model: first add `WW = x > 1 ? r0[x - 2] : W` beside the other neighbour values, then compute `eq = (W==N) | (N==NE)<<1 | (W==NW)<<2 | (NW==N)<<3 | (W==WW)<<4 | (N==NN)<<5` over the plane values `cm_model` already computes and `keq = eq * 16 + qE`; set `NMODEL 6`, append `keq` to `kk`, and extend `code_bit` by one counter (sixth stretched counter and weight in the dot, bias from slot 6, the SIMD input vector becomes `{s0, s1, s2, s3, s4, s5, 64, 0}`, counter updated like the others). Done when: `python research/ab.py size build/research/try-r6.c synth -v` decodes every image.
- [x] In five more fresh copies (`build/research/try-r0.c` to `try-r4.c`), replace one existing model's context in turn: `kk[j] = keq` for `j` from 0 to 4, the same `WW` definition added, nothing else changed. Measure all six variants on `synth`, `photo`, and `tp`. If a winner emerges under the Acceptance rule, time it with `python research/ab.py ab src/cm.c <winner> -n5 -t1 measure kodim01` and `-t3`, repeated until two runs agree on the decode ratio. Done when: every variant's summary line is quoted and (winner only) the agreed decode ratios are quoted. (Done 2026-10-27: all six variants measured on synth/photo/tp; synth geomean r6 +0.48%, r0 +2.86%, r1 +0.36%, r2 -0.05%, r3 +6.9%, r4 +4.8% vs baseline 0.5883. No winner, so no timing runs. Full lines in `docs/reviews/01-compression/D01-T02-s5.md` Measurements.)
- [x] Record the result in `docs/research.md` section 3, and save a winner as `research/variants/repeat-context.patch` with `diff -u --label a/src/cm.c --label b/src/cm.c src/cm.c <winner file> > research/variants/repeat-context.patch` (`diff` exits 1 when the files differ, the expected case) verified with `git apply --check`; if no variant clears the bar, record all six rejections and save no patch. Done when: the table row exists, and the patch exists exactly when the bar was cleared. (Done 2026-10-27: rejection row added in section 3.3; `research/variants/repeat-context.patch` absent, as no variant cleared the bar.)
- [x] Commit: `"research: measure an exact-repeat context"`

**Test checkpoint:** the quoted `ab.py size synth`, `photo`, and `tp` lines for all six variants against the baseline, with the `synth` geometric means and the `tp` verdict; if a winner cleared the bar, its agreed `-t1`/`-t3` decode ratios and `git apply --check research/variants/repeat-context.patch` exiting 0, else the quoted rejection record and no patch file.

> **Verified:** 2026-10-27 | §5 | 4b275760aa37a7ebf5dafd71c534de649b6f1099
> **Review:** `docs/reviews/d01-t02-s5.json`
## 6. The Ceiling of Encoder-Chosen Parameters

> **Started:** 2026-10-27T19:14:01Z

An encoder that tries a few parameter sets per channel and signals the best in the header keeps the decoder's cost unchanged. Before building it, measure its ceiling: the sum over images of the best fixed candidate per image. Bar: a ceiling at least 0.3% below the `photo` total; otherwise the idea is recorded as not worth a header byte.

**Acceptance:** the ceiling is the per-image minimum over the four candidates (the shipped baseline included) summed over `photo`. It clears the bar when that sum is at least 0.3% below the baseline `photo` total. This section measures an oracle bound only: no codec variant is a winner, no patch is saved, and no timing applies (the candidates change constants, not the decode path; the encoder search itself is judged by the follow-up section if one is filed).

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T02-s6.md`):** the WP candidate must change both weight implementations (the `ERRW` table generation in `init_tables`, 13/12 today, and the `maxw` fallback in `cm_model`), or it measures a hybrid predictor; `research/ceiling.py` validates its inputs (exactly the `photo` names from `research/ab.py`, no missing or duplicate rows, consistent JPEG XL sizes across files) and carries a `--self-test` covering known minima, percentages, ties, winner counts, and the exact 0.3% boundary; and the positive branch routes to `D01 T05 §2`, which already owns the encoder search and depends on this section, instead of filing a duplicate section.

**Corrected 2026-10-27:** §6 as written leaves the candidate set ambiguous, so before building: the four candidates are the shipped baseline (`src/cm.c`: LMS step 10, WP maxima `{13, 12, 12, 12}`), LMS step 6, LMS step 16, and WP maxima `{16, 10, 10, 10}`, each variant a fresh copy of `src/cm.c` in `build/research/` (`try-c6.c`, `try-c16.c`, `try-cw.c`) changing exactly one constant (the LMS sign step `ferr > 0 ? 10 : -10` in `cm_code`, or the `maxw` table in `cm_model`). The ceiling script is a tracked tool at `research/ceiling.py` taking the four `-v` outputs; item 3 names the follow-up filing rule and the negative branch.

- [x] Build the three variants (`build/research/try-c6.c` LMS step 6, `build/research/try-c16.c` LMS step 16, `build/research/try-cw.c` WP maxima `{16, 10, 10, 10}` in both places the weights live: the `ERRW` generation in `init_tables` and the `maxw` fallback in `cm_model`), the LMS variants each changing exactly one constant from `src/cm.c` and the WP variant changing exactly those two. Done when: `python research/ab.py size <variant> photo -v` runs for each and decodes every image, and the baseline `photo -v` output is captured the same day. (Done 2026-10-27: `photo -v` captured for baseline and all three variants under `build/research/s6/`; totals LMS 6 30,864,012, LMS 16 30,869,437, WP alt 30,891,641 vs baseline 30,865,211.)
- [x] Write `research/ceiling.py`: it reads the baseline and variant `-v` outputs, validates that each contains exactly the `photo` image names from `research/ab.py` with no missing or duplicate rows and consistent JPEG XL sizes across files (refusing otherwise), computes the per-image minimum summed over `photo` (the ceiling), its percentage against the baseline total and against the summed JPEG XL e7 sizes, and the per-image winner counts; it prints all three. A `--self-test` mode checks known minima, percentages, ties, winner counts, and the exact 0.3% boundary on synthetic inputs. Done when: the script prints the ceiling total and the per-image winner counts, and `--self-test` passes. (Done 2026-10-27: prints ceiling 30,859,686, +0.018% vs baseline, winner counts baseline=9, try-c6=15, try-c16=11, try-cw=4; `--self-test` passes.)
- [x] Record the ceiling in `docs/research.md` section 3 (with the candidate list); the encoder search itself is already owned by `D01 T05 §2` (which depends on this section), so file nothing new either way: if the ceiling clears the bar, the row records that `D01 T05 §2` proceeds; if it does not, the row records the ceiling as not worth a header byte and `D01 T05 §2` will strike its items. Done when: the table row exists and names the `D01 T05 §2` consequence. (Done 2026-10-27: row added in section 3.1 recording the ceiling as not worth a header byte and naming the `D01 T05 §2` consequence; nothing filed.)
- [x] Commit: `"research: measure the ceiling of encoder-chosen parameters"`

**Test checkpoint:** `python research/ceiling.py` output quoted beside the baseline total, with the winner counts, `python3 research/ceiling.py --self-test` passing, and the `D01 T05 §2` consequence named in the section 3 row.

> **Verified:** 2026-10-27 | §6 | f0bd7d74b2ebb9fe1b15868e9a8310034c32e749
> **Review:** `docs/reviews/d01-t02-s6.json`
## 7. The Accepted Variants Measured Together

> **Started:** 2026-10-27T21:38:42Z

**Needs:** Idle machine (timing)

Gains measured one at a time do not simply add: two models can capture the same redundancy. Before the port, the accepted variants of §1 to §6 are combined and measured once, so `D01 T03 §1` ports a measured whole.

**Acceptance:** the combination ships to `D01 T03 §1` when its `photo` total beats the baseline (30,865,211), its `tp` total is no worse than the baseline (the protocol's held-out condition), and no part is dropped by the removal rule in item 2. Either way, the result is recorded as a row in `docs/research.md` section 3: an accepted combination's row names `version-3.patch`; a rejected one records the numbers, produces no patch, and `D01 T03 §1` (which already handles the nothing-accepted case) strikes its items. Decode timing is measured and recorded; the port section re-verifies speed and the JPEG XL clause on the shipped codec, and §1's JPEG XL evidence carries to the combination only if the combined file is byte-identical to §1's measured variant (required by item 1 for this single-patch combination).

**Corrected 2026-10-27 (plan review `docs/reviews/01-compression/D01-T02-s7.md`):** the Acceptance now has explicit accepted/rejected branches (a rejected combination produces no patch, records its row, and leaves `D01 T03 §1` to strike its items), the protocol's `tp` held-out condition, a section 3 row on the successful path too (the file's Outcome requires one for every section), and byte-identity with §1's variant is required, not merely recorded (a mismatch means the patch was misapplied and must be resolved before measuring).

**Corrected 2026-10-27:** of §1 to §6 only §1 was accepted, so the combination is exactly one patch (`research/variants/models-6-7.patch`); item 1 names that and requires the combined file to be diffed against §1's measured variant (`build/research/try.c`) so carried evidence is justified. Item 2's timing is repeated until two runs agree, per the `Needs` rule. Item 3 names the `diff --label` recipe for `version-3.patch` (regenerated from the combined file, not copied from §1's patch) and the exact place parts are listed (`D01 T03 §1`'s context).

- [ ] Apply every patch under `research/variants/` (only `models-6-7.patch` exists; §2 to §6 accepted nothing) to a fresh copy of `src/cm.c` in `build/research/combined.c`, resolving overlaps. Diff the result against §1's measured variant `build/research/try.c`: they must be byte-identical (this combination is that one patch); resolve any mismatch before proceeding. Done when: `python research/ab.py size build/research/combined.c sub` decodes every image and the diff verdict is recorded.
- [ ] Measure `photo`, `tp`, `synth`, and the `-t1`/`-t3` decode ratios (`python research/ab.py ab src/cm.c build/research/combined.c -n5 -t1 measure kodim01` and `-t3`, repeated until two runs agree). Drop any variant whose removal costs less than 0.05% on `photo` (with one part, removal cost is the part's whole gain). Done when: the combined summary lines, the agreed decode ratios, and the drop decision are quoted.
- [ ] Record the result in `docs/research.md` section 3 and, if the combination is accepted, save it as `research/variants/version-3.patch` with `diff -u --label a/src/cm.c --label b/src/cm.c src/cm.c build/research/combined.c > research/variants/version-3.patch` (`diff` exits 1 when the files differ, the expected case), verify `git apply --check research/variants/version-3.patch` exits 0 on a fresh copy, and list its parts in `D01 T03 §1`'s context paragraph in `todo/01-compression/TODO-03-format-version-3.md`. If the combination is rejected, record the rejection row, save no patch, and list no parts. Done when: the table row exists, and the patch and parts list exist exactly when the combination was accepted.
- [ ] Commit: `"research: measure the accepted variants together for format version 3"`

**Test checkpoint:** the combined `ab.py size photo`, `tp`, and `synth` lines against the baseline (photo against the 2026-09-29 figure 0.9712), the `tp` held-out verdict, the agreed decode ratios, and (accepted only) `git apply --check research/variants/version-3.patch` exiting 0; on rejection, the quoted rejection record and no patch file.

## Verification

- [ ] Every section's row in `docs/research.md` section 3 exists and matches its stamp
- [ ] `python scripts/todo-graph.py validate` clean
