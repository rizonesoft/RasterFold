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
|   1   |   §1    | A sixth and seventh context model             | --         |  [ ]   |
|   2   |   §2    | Two adaptation speeds per context             | --         |  [ ]   |
|   3   |   §3    | A second mixer weight set                     | --         |  [ ]   |
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

## 2. Two Adaptation Speeds per Context

**Needs:** Idle machine (timing)

The counters slow to 1/61.5 once a row has been visited 60 times. A second, fast counter per node (fixed rate 1/16) reacts to local change, and the mixer learns when to trust each (the two-rate state of lpaq and paq8). Bar: as §1, and decode no more than 10% slower per 0.1% gained.

- [ ] In `build/research/try.c`, double `NODES` to 64, keep slot `node` as the adaptive counter and slot `node + 32` as a fast counter starting at 32768 updated by `c += ((target - c) * 4096) >> 16`, and feed model 0's fast counter to the mixer as a sixth input. Done when: `python research/ab.py size build/research/try.c sub` decodes every image.
- [ ] Measure `photo`, `tp`, and `synth`, then extend the fast counters to models 0 and 1, then to all five (`WSTRIDE` 16). Done when: each variant's summary line and its `-t1` decode ratio are quoted.
- [ ] Record the result in `docs/research.md` section 3, and save a winner as `research/variants/fast-counters.patch`. Done when: the table row exists, and the patch exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure two adaptation speeds per context"`

**Test checkpoint:** the quoted `ab.py size` and `ab` lines against the baseline.

## 3. A Second Mixer Weight Set

**Needs:** Idle machine (timing)

Mixer weights depend only on `wsel` (the error energy and the G residual class). Averaging two mixers whose weight sets are chosen by different contexts is a standard gain in paq-style coders. Bar: as §2.

- [ ] In `build/research/try.c`, add a second weight table the size of `wt`, select its row by `wsel2 = k2 % 128`, compute a second dot product in `code_bit`, use `x = (x1 + x2) >> 1` before `SQUASH`, and update both rows with the same error. Done when: `python research/ab.py size build/research/try.c sub` decodes every image.
- [ ] Measure `photo`, `tp`, `synth`, and the `-t1` and `-t3` decode ratios. Done when: the summary lines and ratios are quoted.
- [ ] Record the result in `docs/research.md` section 3, and save a winner as `research/variants/second-mixer.patch`. Done when: the table row exists, and the patch exists exactly when the bar was cleared.
- [ ] Commit: `"research: measure a second mixer weight set"`

**Test checkpoint:** the quoted `ab.py size` and `ab` lines against the baseline.

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
