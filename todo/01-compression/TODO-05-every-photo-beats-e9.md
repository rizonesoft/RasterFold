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

- [`docs/research.md`](../../docs/research.md) -- sections 4.5 (encoder-chosen parameters) and 4.6 (learning cost on small images)
- -> XREF: D01 T02 §6 -- the ceiling of encoder-chosen parameters, which decides whether §2 builds the search
- -> XREF: D01 T03 §1 -- format version 3, which §2 extends before the 1.0 release

## Outcome

- At least 34 of 39 photos in `photo` and 8 of 10 photos in `tp` are below their JPEG XL e9 size, and no photo in `photo` or `tp` is more than 1.0% larger than e9.
- Encode takes at most 0.3 of JPEG XL e7's time at one thread.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (the encoder chooses its parameters itself); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T05 §2; reverse=applicable @ D01 T05 §2

**Adjacency rationale:** The parameter byte changes what the encoder exports and the decoder imports.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Why the losing photos lose | -- |  [ ]   |
|   2   |   §2    | Encoder-chosen parameters in version 3 | §1, D01 T02 §6, D01 T03 §1 |  [ ]   |

---

## 1. Why the Losing Photos Lose

> **Started:** 2026-11-06T08:17:00Z

**Needs:** ffmpeg with libjxl (benchmarks)

**Corrected 2026-11-06:** At the Milestone 6 codec (`b3eb341`), 12 of 39 `photo` images (`kodim16`, `kodim17`, `ic_nightshot_iso_1600`, `kodim12`, `ic_flower_foveon`, `kodim09`, `kodim10`, `kodim01`, `kodim06`, `kodim21`, `kodim08`, `kodim24`) and 3 of 10 `tp` images (`tp_nightshot_iso_1600_crop`, `tp_flower_foveon_crop`, `kodim01`; 14 distinct images) are above JPEG XL e9, with 7 `photo` images and 2 `tp` images above `1.0100` of e9. `research/bench.c` adds `--breakdown` (accepted alongside `-r<N>`, `-t<N>`, and `-p` with zero runtime overhead when `--breakdown` is omitted) by compiling a second, instrumented CM encode pass (`bd_cm_encode`, included from `../src/cm.c` with `renc_bit` macro-wrapped) that prints each channel's (`G`, `R-G`, `B-G`) exact flushed stream size across strips and the byte and percentage split across the four decision kinds (`zero` node `1`, `sign` node `2`, `exp` nodes `3..8`, and `mant` adaptive nodes `9..15` plus raw bits from `code_raw`), verified by a unit test in `tests/test_research_refs.py`. Running `--breakdown` across all 14 losing photos and 6 winning reference photos (`measure`, `kodim03`, `kodim20`, `kodim23`, `ic_cathedral`, `ic_artificial`) separates the losses into two causes recorded in `docs/research.md` sections 4.5 and 4.12 and filed as new sections of this file (and wired into `D05 T01 §7` in `research/milestones.py` and `todo/implementation-plan.md`).

- [ ] List every photo above JPEG XL e9 with `python research/ab.py size photo -v` and `tp -v` at the current codec. Done when: the list with ratios is quoted.
- [ ] Break each losing photo's bytes down per channel and per decision kind (zero flag, sign, exponent, mantissa) with a `--breakdown` mode added to `research/bench.c`, beside a photo that wins. Done when: the breakdown table names where the extra bytes go. Cheaper substitute: guessing from the picture content.
- [ ] Record the findings in `docs/research.md` section 4.5 and file each fix the breakdown points at through `plan-experiment`. Done when: the findings are recorded and every filed section is in this milestone's phase.
- [ ] Commit: `"research: explain the photos that lose to JPEG XL e9"`

**Test checkpoint:** the breakdown table for at least three losing photos and one winning photo, with the filed follow-ups named.

**Freeze check:** `D01 T05 §1` modifies only `research/bench.c`, `tests/test_research_refs.py`, `docs/research.md`, and the TODO/milestone plan files (`todo/01-compression/TODO-05-every-photo-beats-e9.md`, `todo/01-compression/INDEX.md`, `research/milestones.py`, `todo/05-milestones/TODO-01-beat-jxl-and-webp.md`, `todo/implementation-plan.md`); `src/`, `spec/FOLD.md`, `tests/rfi_ref.py`, and `KAT_FOLD` / `KAT_FOLD3` are unchanged.

## 2. Encoder-Chosen Parameters in Version 3

**Needs:** Idle machine (timing)

Builds the per-channel parameter search `docs/research.md` section 4.5 designs, if `D01 T02 §6` measured its ceiling at 0.3% or more; otherwise this section records that and strikes its items.

- [ ] Add one parameter byte per channel to the version 3 payload and the search to `rf_cm_encode` (each candidate coded once, the smallest kept per channel). Done when: every candidate's file decodes, and the decoder imports each back to identical pixels (the reverse of the export).
- [ ] Write the parameter byte into `tests/rfi_ref.py`, `spec/FOLD.md`, and `KAT_FOLD3`. Done when: codec and oracle agree on every case.
- [ ] Measure `photo`, `tp`, and the encode ratio with `python research/vsjxl.py -n5`. Done when: the losing photos' ratios and the encode time are quoted.
- [ ] Commit: `"codec: version 3 carries encoder-chosen parameters per channel"`

**Test checkpoint:** `python tests/test_correctness.py` passes; the per-image ratios of the formerly losing photos against JPEG XL e9; encode at most 0.3 of JPEG XL e7's time.

**Freeze check:** `KAT_FOLD` (version 2) decodes unchanged; the operator's standing approval (`D99 T01 §3`) covers extending the unreleased version 3.

## Verification

- [ ] `pwsh scripts/check-all.ps1` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
