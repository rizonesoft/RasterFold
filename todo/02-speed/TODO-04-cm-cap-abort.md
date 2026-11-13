---
schema_version: 1
id: cm-cap-abort
domain: 02-speed
status: draft
title: "TODO-04 -- Finer-Grained CM Cap-Abort Check for Small Palette Caps"
depends_on: []
frozen: true
track: S4
---

# TODO-04 -- Finer-Grained CM Cap-Abort Check for Small Palette Caps

> **Goal:** Eliminate the 32-row step-function overshoot in `run_pipelined` (`src/cm.c`) when a small palette or screen-content trial cap (`m->cap < 16384`) has already been exceeded, reducing wasted Method 1 trial encode work on synthetic images while keeping every `.fold` output byte identical.

> [!IMPORTANT]
> **Current state (verified 2026-11-13):** In `src/cm.c`, `run_pipelined` checks `tot > m->cap` on a 32-row cadence (`(y & 31u) == 0u`), so when Method 4 or Method 6 establishes a small cap (such as ~1.4 KB on `syn_ui`), single-thread CM trial encoding can run up to 31 extra rows past the cap before aborting.
<!-- claim: exists src/cm.c -->

## Inputs

- [`src/cm.c`](../../src/cm.c) -- `run_pipelined` and strip/channel cap-abort checks during Method 1 trial encoding
- [`research/abpool.py`](../../research/abpool.py) -- live shared-host A/B encode timing harness (`RULE_2026_11_13`)
- -> XREF: D09 T01 §4 -- release backlog review for `rasterfold-v1.0.0` that promoted `B-003`

## Outcome

- Either a finer-grained cap check (every 4 or 8 rows when `m->cap < 16384`) ships in `src/cm.c` with proven byte-identical `.fold` output across all corpus sets (`photo`, `tp`, `synth`, `screens`, `heldout`) and a measured encode-time improvement on `synth` with no `photo` encode regression, or a negative result is recorded in `docs/research.md` if the check saves below 2% on `synth` encode time.

**Adjacency:** all=not-applicable (an internal encoder early-abort cadence check that preserves byte-identical `.fold` output)

**Adjacency rationale:** Whenever `tot > m->cap` holds, the completed CM stream is strictly larger than an already-encoded candidate method (`pal` or `sc`) because range-coder output length is non-decreasing across rows; checking earlier changes no chosen method and no written byte.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Finer-grained CM cap-abort check for small caps | D05 T01 §5, D05 T01 §10 |  [ ]   |

---

## 1. Finer-Grained CM Cap-Abort Check for Small Caps

**Origin:** discovered run=36a212a58ece 2026-11-13 -- promoted from `B-003` during `D09 T01 §4` release backlog review

-> SOURCE: D05-T01-S5-cm-cap-abort

**Needs:** Idle machine (timing)

When a palette or screen-content method has already produced a small compressed payload (`m->cap < 16384`), checking `tot > m->cap` every 4 or 8 rows instead of every 32 rows in `src/cm.c` (`run_pipelined` and single-strip trial encode, verifying exact byte-identity between `--threads 1` and `--threads 3, 6, 9` when `tot` is a lower bound on final stream bytes) cuts wasted CM trial rows without touching normal photograph caps. Stop condition: keep the change in `src/cm.c` only if every file across `photo`, `tp`, `synth`, `screens`, and `heldout` is 100% byte-identical at `--threads 1` and `--threads 3`, `synth` 1-thread encode time improves by at least `2%` under `research/abpool.py` (`RULE_2026_11_13`), and `photo` encode time does not regress beyond `1.01x`; otherwise revert `src/cm.c` and record the measured figures in `docs/research.md`. Care: ensure any early abort uses a conservative lower bound on final flushed stream bytes so a stream that could still finish `<= m->cap` is never aborted, and prove thread-count byte identity across boundary caps just above and just below final CM size.

- [ ] Prototype the `m->cap < 16384` 4-row / 8-row conservative lower-bound cap check in `src/cm.c`, add a boundary cap-equivalence test in `tests/test_correctness.py` (an image where Method 1 beats or ties palette by 1 to 8 bytes must never be falsely aborted, and `--threads 1, 3, 6, 9` remain byte-identical), and verify `python3 research/ab.py size src/cm.c all` is byte-for-byte unchanged on every set. Done when: correctness and byte-identity hold across every corpus image and thread count. Cheaper substitute: comparing raw unflushed byte count without accounting for pending carry/flush bounds or multi-thread determinism.
- [ ] Measure `synth` and `photo` encode timing (`-t1` and `-t3`) with `python3 research/abpool.py` under `RULE_2026_11_13`, apply the stop condition, update `docs/research.md` (`python3 scripts/build-docs.py --write`), and verify `python3 scripts/check-all.py --all-targets`. Done when: all gates exit 0.
- [ ] Commit: `"speed: tighten CM trial cap-abort cadence for small caps"`

**Test checkpoint:** `research/abpool.py` contiguous tail-pool encode timing on `synth` and `photo` quoted, `python3 research/ab.py size` zero-byte delta verified, and `python3 tests/test_correctness.py` passes.

**Freeze check:** every vector in `testdata/compat/` (`KAT_FOLD`, `KAT_FOLD3`, `KAT_PAL4`, `KAT_SC6`) and every corpus image encodes and decodes to identical bytes across `--threads 1, 3, 6, 9`.

## Verification

- [ ] `python3 tests/test_correctness.py` exits 0
- [ ] `python3 scripts/check-all.py --all-targets` exits 0
- [ ] `python3 scripts/todo-graph.py validate` clean
