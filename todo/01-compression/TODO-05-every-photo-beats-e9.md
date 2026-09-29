---
schema_version: 1
id: every-photo-beats-e9
domain: 01-compression
status: draft
title: "TODO-05 -- Every Photograph Beats JPEG XL e9"
depends_on: []
frozen: true
track: C5
---

# TODO-05 -- Every Photograph Beats JPEG XL e9

> **Goal:** No photograph in the research sets is larger than JPEG XL effort 9, closing the last per-image losses after milestone 4, with encode time still at most 0.3 of JPEG XL e7's.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** On 2026-09-29, 28 of 39 photos and 7 of 10 test-pack photos are below JPEG XL e9; the worst are kodim16 (1.0193 of e9) and `tp_nightshot_iso_1600_crop` (1.0131). Milestone 4's experiments (`D01 T02 §1` to `§7`) will move these figures before this file runs, so §1 re-measures first. `D01 T02 §6` measures the ceiling of encoder-chosen parameters.
<!-- claim: exists research/ab.py -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- sections 4.5 (encoder-chosen parameters) and 4.6 (learning cost on small images)
- -> XREF: D01 T02 §6 -- the ceiling of encoder-chosen parameters, which decides whether §2 builds the search
- -> XREF: D01 T03 §1 -- format version 3, which §2 extends before the 1.0 release

## Outcome

- Every photo in `photo` and `tp` is below its JPEG XL e9 size.
- Encode takes at most 0.3 of JPEG XL e7's time.

**Adjacency:** list=not-applicable (a codec has no records to browse); document=not-applicable (no printed output); settings=not-applicable (the encoder chooses its parameters itself); reporting=not-applicable (no new report); notifications=not-applicable (no new message); permissions=not-applicable (file safety unchanged); audit=not-applicable (git history is the audit); exchange=applicable @ D01 T05 §2; reverse=applicable @ D01 T05 §2

**Adjacency rationale:** The parameter byte changes what the encoder exports and the decoder imports.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Why the losing photos lose | -- |  [ ]   |
|   2   |   §2    | Encoder-chosen parameters in version 3 | §1, D01 T02 §6, D01 T03 §1 |  [ ]   |

---

## 1. Why the Losing Photos Lose

**Needs:** ffmpeg with libjxl (benchmarks)

- [ ] List every photo above JPEG XL e9 with `python research/ab.py size photo -v` and `tp -v` at the current codec. Done when: the list with ratios is quoted.
- [ ] Break each losing photo's bytes down per channel and per decision kind (zero flag, sign, exponent, mantissa) with a `--breakdown` mode added to `research/bench.c`, beside a photo that wins. Done when: the breakdown table names where the extra bytes go. Cheaper substitute: guessing from the picture content.
- [ ] Record the findings in `docs/research.md` section 4.5 and file each fix the breakdown points at through `plan-experiment`. Done when: the findings are recorded and every filed section is in this milestone's phase.
- [ ] Commit: `"research: explain the photos that lose to JPEG XL e9"`

**Test checkpoint:** the breakdown table for at least three losing photos and one winning photo, with the filed follow-ups named.

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
