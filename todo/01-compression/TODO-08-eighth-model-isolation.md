---
schema_version: 1
id: eighth-model-isolation
domain: 01-compression
status: draft
title: "TODO-08 -- Isolate the Eighth-Model Match Context Gain"
depends_on: []
frozen: true
track: C8
---

# TODO-08 -- Isolate the Eighth-Model Match Context Gain

> **Goal:** Determine by controlled A/B measurement on the research corpus whether the `-0.149%` `photo` / `-0.121%` `tp` compression gain observed in `D01 T04 §2`'s rejected 8-model match experiment came from the match context itself or from adding an eighth mixer slot, and whether a 9-slot SIMD mixer keeps decode time within budget for a future format version.

> [!IMPORTANT]
> **Current state (verified 2026-11-13):** `src/cm.c` uses `NMODEL = 7` and `WSTRIDE = 8` (`7` context models plus `1` predictor slot) in frozen format version 3. `D01 T04 §2` measured an eighth match-context model (`photo -0.149%`, `tp -0.121%`) that missed its `synth` acceptance bar and ran at roughly `1.6x` decode time because `WSTRIDE` widened from `8` to `16` without a dedicated 9-slot SIMD mixer.
<!-- claim: count "^#define NMODEL 7$" src/cm.c = 1 -->
<!-- claim: count "^#define WSTRIDE 8 " src/cm.c = 1 -->

## Inputs

- [`docs/research.md`](../../docs/research.md) -- "Already tried" entry for `D01 T04 §2` and section 2 measurement protocol
- [`src/cm.c`](../../src/cm.c) -- 7-model context mixer and `WSTRIDE = 8` SIMD dot/update hot loop
- [`research/ab.py`](../../research/ab.py), [`research/abpool.py`](../../research/abpool.py) -- corpus size and shared-host alternating A/B timing runners
- -> XREF: D09 T01 §4 -- release backlog review for `rasterfold-v1.0.0` that promoted `B-002`

## Outcome

- `docs/research.md` records three controlled variant measurements against the `NMODEL = 7` baseline on `photo`, `tp`, and `synth` (a constant-context eighth model isolating mixer width, the `D01 T04 §2` match context replacing the weakest existing model at `NMODEL = 7`, and an 8-model + 1-predictor SIMD layout), with either a concrete format-version-4 candidate handed forward or a proven negative result recorded in `docs/research.md` and `research/variants/`.

**Adjacency:** all=not-applicable (a research variant isolation experiment; format version 3 remains frozen unless a future format-version section ports a winner)

**Adjacency rationale:** This file measures candidate variants in `research/variants/` without changing what frozen format version 3 writes or what the CLI/API expose.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Isolate mixer width vs match context and measure 9-slot SIMD cost | D01 T04 §2, D05 T02 §2 |  [ ]   |

---

## 1. Isolate Mixer Width vs Match Context and Measure 9-Slot SIMD Cost

**Origin:** discovered run=36a212a58ece 2026-11-13 -- promoted from `B-002` during `D09 T01 §4` release backlog review

-> SOURCE: D01-T04-S2-photo-gain

**Needs:** Idle machine (timing)

`D01 T04 §2`'s rejected 8-model match experiment combined two changes at once: a new match context and widening the mixer from 8 active inputs (`7` models + `1` predictor, `WSTRIDE = 8`) to 9 (`WSTRIDE = 16`). Isolate the two effects in `research/variants/` without touching shipped `src/cm.c` (format version 3 is frozen from `rasterfold-v1.0.0`): baseline is shipped `src/cm.c` (`NMODEL = 7`). Stop condition: record a positive candidate for a post-v3 format version only if a variant improves `photo` total by at least `0.10%` (`ratio <= 0.9990` of baseline), improves or holds `tp` (`<= 1.0000`), regresses no `photo` image by more than `0.15%`, and holds 3-thread decode time within `1.08x` of `src/cm.c` under `research/abpool.py` contiguous tail pools (`RULE_2026_11_13`); otherwise keep `src/cm.c` untouched and ship the measured isolation table and variant patches in `research/variants/` and `docs/research.md`.

- [ ] Build three standalone variants under `research/variants/` and measure `python3 research/ab.py size <variant> all` against `src/cm.c`: (a) `eighth-const.c` (`NMODEL = 8` with a constant zero-hash eighth model, isolating mixer-slot capacity), (b) `match-replace7.c` (`NMODEL = 7` replacing the weakest of `k0..k6` with the `D01 T04 §2` match context at `WSTRIDE = 8`), and (c) `eighth-match-simd9.c` (`NMODEL = 8` with an `8 + 1` SIMD dot/update specializations avoiding generic 16-lane scalar/padded waste). Done when: `ab.py size` totals, geometric means, and worst images on `photo`, `tp`, and `synth` are captured for all three variants. Cheaper substitute: measuring only `photo` or skipping the `NMODEL = 7` replacement control.
- [ ] Measure alternating A/B decode timing on `photo` (`-t1` and `-t3`) with `python3 research/abpool.py` under `RULE_2026_11_13` for the best-compressing variant against `src/cm.c`, record the full size + speed isolation table in `docs/research.md` ("Already tried" or candidate design), and regenerate `docs/research-summary.md` via `python3 scripts/build-docs.py --write`. Done when: `python3 scripts/drift-check.py` exits 0 and `src/cm.c` remains byte-identical to `rasterfold-v1.0.0`.
- [ ] Commit: `"research: isolate eighth-model match context gain and 9-slot SIMD cost"`

**Test checkpoint:** `python3 research/ab.py size` output for all three variants across `photo`, `tp`, and `synth` plus `research/abpool.py` contiguous tail-pool decode timing quoted, and `python3 tests/test_correctness.py -k compat-corpus` passes.

**Freeze check:** `src/cm.c`, `spec/FOLD.md`, and `tests/rfi_ref.py` are untouched; `KAT_FOLD3` and every vector in `testdata/compat/` decode and encode identically.

## Verification

- [ ] `python3 tests/test_correctness.py` exits 0
- [ ] `python3 scripts/drift-check.py` exits 0
- [ ] `python3 scripts/todo-graph.py validate` clean
