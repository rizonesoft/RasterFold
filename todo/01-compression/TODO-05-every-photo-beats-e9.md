---
schema_version: 1
id: every-photo-beats-e9
domain: 01-compression
status: active
title: "TODO-05 -- Every Photograph Beats JPEG XL e9"
depends_on: []
frozen: true
track: C5
---

# TODO-05 -- Every Photograph Beats JPEG XL e9

> **Goal:** At least 34 of 39 photos and 8 of 10 test-pack photos are smaller than JPEG XL effort 9 and none is more than 1.0% larger (Milestone 7's target as revised by the operator on 2026-10-30, `D99 T01 §4`), closing most of the per-image losses left after milestone 4, with encode time still at most 0.3 of JPEG XL e7's at one thread.

> [!IMPORTANT]
> **Current state (verified 2026-11-06):** At Milestone 6 (`b3eb341`, format version 3 with 7 context models, learned gated modular prediction, and warm-started horizontal strips), `python3 research/ab.py size src/cm.c photo -v` measures `total=30868254` (`0.9830` of JPEG XL e9 `31,400,714`, geomean `0.9881`, `27 of 39` photos below e9, 12 above e9, 7 of which are `> 1.0100` of e9, worst `kodim16` `371,397 / 364,606 = 1.0186`), and `python3 research/ab.py size src/cm.c tp -v` measures `total=6656840` (`0.9876` of e9 `6,740,249`, geomean `0.9866`, `7 of 10` test-pack photos below e9, 3 above e9, 2 of which are `> 1.0100` of e9, worst `tp_nightshot_iso_1600_crop` `1,205,267 / 1,187,904 = 1.0146`). `D01 T02 §6` (`f0bd7d74b2ebb9fe1b15868e9a8310034c32e749`) measured the ceiling of encoder-chosen parameters at `-0.018%` (`< 0.3%`), so §2 takes its rejection path while §1 diagnoses the 14 losing photos across `photo` and `tp` and files the follow-up experiments into Phase 6.
<!-- claim: exists research/ab.py -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- sections 4.5 (encoder-chosen parameters and why the losing photos lose), 4.6 (learning cost on small images), and 4.12 (strip-adaptation priors, collision-reduced contexts, and noise-damped prediction)
- -> XREF: D01 T02 §6 -- the ceiling of encoder-chosen parameters, which decides whether §2 builds the search
- -> XREF: D01 T03 §1 -- format version 3, which §2 extends before the 1.0 release

## Outcome

- At least 34 of 39 photos in `photo` and 8 of 10 photos in `tp` are below their JPEG XL e9 size, and no photo in `photo` or `tp` is more than 1.0% larger than e9.
- Encode takes at most 0.3 of JPEG XL e7's time at one thread.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (the encoder chooses its parameters itself); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T05 §4; reverse=applicable @ D01 T05 §4

**Adjacency rationale:** The parameter byte (§2, if built) and the version 3 model, predictor, and strip refinements (§4) change what the encoder exports and the decoder imports.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Why the losing photos lose | -- |  [x]   |
|   2   |   §2    | Encoder-chosen parameters in version 3 | §1, D01 T02 §6, D01 T03 §1 |  [ ]   |
|   3   |   §3    | Strip-adaptation priors, collision-reduced contexts, and noise-damped prediction | §1, §2 |  [ ]   |
|   4   |   §4    | Port Milestone 7 CM improvements into version 3 | §3 |  [ ]   |

---

## 1. Why the Losing Photos Lose

> **Started:** 2026-11-06T08:17:00Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Corrected 2026-11-06:** At the Milestone 6 codec (`b3eb341`), 12 of 39 `photo` images (`kodim16`, `kodim17`, `ic_nightshot_iso_1600`, `kodim12`, `ic_flower_foveon`, `kodim09`, `kodim10`, `kodim01`, `kodim06`, `kodim21`, `kodim08`, `kodim24`) and 3 of 10 `tp` images (`tp_nightshot_iso_1600_crop`, `tp_flower_foveon_crop`, `kodim01`; 14 distinct images) are above JPEG XL e9, with 7 `photo` images and 2 `tp` images above `1.0100` of e9. `research/bench.c` adds `--breakdown` (accepted alongside `-r<N>`, `-t<N>`, and `-p` with zero runtime overhead when `--breakdown` is omitted) by compiling an instrumented second CM encode pass (`bd_cm_encode_inner`, included from `RF_BENCH_CM_PATH`, defaulting to `../src/cm.c`, with `research/ab.py` passing `-DRF_BENCH_CM_PATH="<cm_src>"` for non-M1 builds and `-DRF_NO_BREAKDOWN=1` for historical Milestone 1 builds, where `--breakdown` exits with code 2). In `bd_cm_encode_inner`, `renc_init` binds each strip's `renc` state to its channel `c in {0, 1, 2}` (`G`, `R-G`, `B-G`) and `renc_bit` accumulates the ideal range-coder sub-interval cost `bits = -log2((double)nr / (double)rc->range)` (`bytes = bits / 8.0`), where `bound = (rc->range >> 12) * p1` and `nr = bit ? bound : (rc->range - bound)` (`src/rc.h:35`), into four decision kinds: `zero` (`node == 1`), `sign` (`node == 2`), `exp` (`3 <= node <= 8`), and `mant` (`9 <= node <= 15` adaptive mantissa bits plus `code_raw` raw bits at `p1 = 2048`, attributed via file-scope `node = 16`). `run_breakdown` verifies that `bd_cm_encode_inner`'s payload is byte-identical to `rf_encode`'s `.fold` payload (`fold + RF_HEADER_SIZE`, length `fold_sz - 32`), sums each channel's exact flushed stream bytes across all `S` strips from the `.fold` stream directory (`stream_G + stream_R-G + stream_B-G == payload_sz - payload_hdr`), and prints per-channel and `ALL` lines reconciling ideal decision bytes `dec`, `flush = stream - dec`, and container + directory overhead `hdr = 32 + 1 + 12*S` (`stream_ALL + hdr == fold_sz`), with `RAW` and `PAL` files printing a `non-CM` summary line. Unit test `test_bench_breakdown_reconciles_channels_kinds_and_variants` in `tests/test_research_refs.py` verifies instrumented-vs-ordinary `.fold` identity, channel/strip totals, all four decision kinds including raw mantissa bits, repetition resets (`-r1` vs `-r2`), thread-count consistency (`-t1` vs `-t3`), `RAW`/`PAL` handling, variant CM source instrumentation, and `-DRF_NO_BREAKDOWN=1` rejection. Running `--breakdown` across all 14 losing photos and 6 winning reference photos (`measure`, `kodim03`, `kodim20`, `kodim23`, `ic_cathedral`, `ic_artificial`) provides normalized channel and decision-kind comparisons and testable hypotheses recorded in `docs/research.md` sections 4.5 and 4.12 and filed as §3 and §4 of this file (and wired into `D05 T01 §7` in `research/milestones.py`, `todo/05-milestones/TODO-01-beat-jxl-and-webp.md`, and `todo/implementation-plan.md`).

- [x] List every photo above JPEG XL e9 with `python research/ab.py size photo -v` and `tp -v` at the current codec. Done when: the list with ratios is quoted.
- [x] Break each losing photo's bytes down per channel and per decision kind (zero flag, sign, exponent, mantissa) with a `--breakdown` mode added to `research/bench.c`, beside a photo that wins. Done when: the breakdown table names where the extra bytes go. Cheaper substitute: guessing from the picture content.
- [x] Record the findings in `docs/research.md` section 4.5 and file each fix the breakdown points at through `plan-experiment`. Done when: the findings are recorded and every filed section is in this milestone's phase.
- [x] Commit: `"research: explain the photos that lose to JPEG XL e9"`

**Test checkpoint:** the full 14-losing plus 6-winning photo breakdown table in `docs/research.md` section 4.5 with normalized channel/decision-kind shares and the filed follow-up sections (`D01 T05 §3` and `D01 T05 §4`) named; `python3 -m unittest tests.test_research_refs -v` passes (`test_bench_breakdown_reconciles_channels_kinds_and_variants`).

**Freeze check:** `D01 T05 §1` modifies only `research/bench.c`, `research/ab.py`, `research/README.md`, `docs/drift.json`, `tests/test_research_refs.py`, `docs/research.md`, and the TODO/milestone plan files (`todo/01-compression/TODO-05-every-photo-beats-e9.md`, `todo/01-compression/INDEX.md`, `research/milestones.py`, `todo/05-milestones/TODO-01-beat-jxl-and-webp.md`, `todo/implementation-plan.md`); `src/`, `spec/FOLD.md`, `tests/rfi_ref.py`, and `KAT_FOLD` / `KAT_FOLD3` are unchanged.

> **Verified:** 2026-11-06 | §1 | 03308c0f7b9561ee05e8ace9afa3887f63300dbb
> **Review:** `docs/reviews/d01-t05-s1.json`
## 2. Encoder-Chosen Parameters in Version 3

**Needs:** Idle machine (timing)

Builds the per-channel parameter search `docs/research.md` section 4.5 designs, if `D01 T02 §6` measured its ceiling at 0.3% or more; otherwise this section records that and strikes its items.

- [ ] Add one parameter byte per channel to the version 3 payload and the search to `rf_cm_encode` (each candidate coded once, the smallest kept per channel). Done when: every candidate's file decodes, and the decoder imports each back to identical pixels (the reverse of the export).
- [ ] Write the parameter byte into `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_FOLD3`. Done when: codec and oracle agree on every case.
- [ ] Measure `photo`, `tp`, and the encode ratio with `python research/vsjxl.py -n5`. Done when: the losing photos' ratios and the encode time are quoted.
- [ ] Commit: `"codec: version 3 carries encoder-chosen parameters per channel"`

**Test checkpoint:** `python tests/test_correctness.py` passes; the per-image ratios of the formerly losing photos against JPEG XL e9; encode at most 0.3 of JPEG XL e7's time.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

## 3. Strip-Adaptation Priors, Collision-Reduced Contexts, and Noise-Damped Prediction

**Origin:** discovered run=69e45b93535e 2026-11-06

**Needs:** Idle machine (timing)

Tests `docs/research.md` section 4.12 in `build/research/try.c` against the Milestone 6 `src/cm.c` baseline (`b3eb341`): empirical counter priors in `chan_init_cold` (`c0_v = (0, 32768, 32768, 30000, 28000, 24000, 18000, 12000)`, `c1_v = (6000, 29000, 29000, 29000, 29000, 30000, 30000, 30000)`), damped adaptation schedule `RATE[0..72]` (`(65536 * 2) / (2 * n + 7)` with `(s->predv != s->base && n >= 2) ? (n - 2) : n`), `choose_strips(h)` (`h >= 1024 ? 3 : 1`) with unhalved `chan_copy_warm` LMS weights and `c == 2` `lms[6] = 20000`, collision-reduced context hashes `k0`, `k2`, `k3`, `k4`, `k5`, `k6`, and `wsel` with `scl = (E >> 2) + 3` and `rq` cross-channel conditioning on `c == 2`, and high-frequency noise damping in `wpred` (`pr[0] = w8 + (((ne8 - n8) * 5) >> 3)`, `pr[1..3]` with weights `13, 9, 6`).

- [ ] Build `build/research/try.c` with the four mechanisms of `docs/research.md` section 4.12 and measure `photo`, `tp`, and `synth` with `python3 research/ab.py size build/research/try.c <set> -v`, plus A/B timing with `python3 research/ab.py ab src/cm.c build/research/try.c measure -n5` at `-t1` and `-t3`. Done when: `photo`, `tp`, and `synth` summary lines, the per-image counts below JPEG XL e9, the worst ratios against e9 and against Milestone 6, and the A/B timing lines are quoted.
- [ ] Stop condition: `photo` total at least `0.5%` smaller than Milestone 6 (`<= 30,713,912`), at least `34 of 39` `photo` images and `8 of 10` `tp` images below JPEG XL e9, no image in `photo` or `tp` above `1.0100` of e9, no image in `photo` or `tp` more than `+0.5%` larger than Milestone 6, and `measure` `-t1`/`-t3` decode within `1.03x` of Milestone 6. Record the result in `docs/research.md` section 4.12 (and section 3.2 if rejected). Done when: the decision and exact figures are recorded in `docs/research.md`.
- [ ] Commit: `"research: measure strip-adaptation priors, context hashes, and noise-damped prediction"`

**Test checkpoint:** `python3 research/ab.py size build/research/try.c photo -v`, `tp -v`, and `synth -v` summary lines and per-image e9 comparison quoted; `python3 research/ab.py ab src/cm.c build/research/try.c measure -n5` at `-t1` and `-t3` quoted.

**Freeze check:** `src/` is not touched in §3; the candidate lives in `build/research/try.c` until §4 ports it.

## 4. Port Milestone 7 CM Improvements into Version 3

**Origin:** discovered run=69e45b93535e 2026-11-06

**Needs:** Idle machine (timing)

Ports the accepted design of §3 (`docs/research.md` section 4.12) into format version 3 across `src/cm.c`, `spec/FOLD.md`, `tests/rfi_ref.py` (written independently from `spec/FOLD.md`), and `KAT_FOLD3` in `tests/test_correctness.py`, keeping format version 2 (`KAT_FOLD`) byte-identical.

- [ ] Port the accepted changes of §3 into `src/cm.c`, update `spec/FOLD.md` (Format version 3's strip count rule, cold/warm channel initialization, `RATE` table, `wpred` sub-predictors, and `k0..k6` / `wsel` context definitions), implement the independent reference oracle in `tests/rfi_ref.py` from `spec/FOLD.md`, and regenerate `KAT_FOLD3` in `tests/test_correctness.py`. Done when: every encoded version 3 file decodes identically across `--threads 1, 3, 6, 9` and matches `tests/rfi_ref.py` byte for byte (the reverse of the export).
- [ ] Run `python3 scripts/check-all.py --all-targets`, `python3 research/ab.py size src/cm.c photo -v`, `tp -v`, and `synth -v`, `python3 research/vsjxl.py -n5` at `-t 1`, and `python3 tests/benchmark.py -n 3 --threads 9` and default threads on `testdata/photos/`. Done when: all gates pass, `>= 34/39` `photo` and `>= 8/10` `tp` are below JPEG XL e9 with `0` images `> 1.0100` of e9, `-t 1` encode on `measure/image.bmp` is `<= 0.3x` JPEG XL e7, and `testdata/photos/` decode time stays below lossless WebP.
- [ ] Commit: `"codec: refine version 3 priors, contexts, strips, and predictor for Milestone 7"`

**Test checkpoint:** `python3 scripts/check-all.py --all-targets` exits 0; `python3 tests/test_correctness.py` passes (including `KAT_FOLD` v2 unchanged and `KAT_FOLD3` v3 oracle byte identity); `python3 research/ab.py size src/cm.c photo -v` and `tp -v` quote `>= 34/39` and `>= 8/10` below e9 with worst `<= 1.0100`; `python3 research/vsjxl.py -n5` quotes `-t 1` encode `<= 0.3x` JPEG XL e7.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending unreleased format version 3 across `src/cm.c`, `spec/FOLD.md`, `tests/rfi_ref.py`, and `tests/test_correctness.py` in one commit.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
