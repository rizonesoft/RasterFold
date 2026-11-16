---
schema_version: 1
id: drift-watched-build-dir
domain: 08-docs
status: draft
title: "TODO-05 -- Drift Gate Watched-Paths Exclusion for Ignored Build Directory"
depends_on: []
track: DOC1
---

# TODO-05 -- Drift Gate Watched-Paths Exclusion for Ignored Build Directory

> **Goal:** Prevent concurrent writes to untracked runtime files inside ignored `build/` (such as `build/claude-campaign-state.json` written by `scripts/campaign_guard.py`) from tripping `scripts/drift-check.py` read-only generator checks (`--check modified build; check mode must be read-only`) while preserving byte-level verification of declared generator outputs under `build/` (such as `build/milestones.json`).

> [!IMPORTANT]
> **Current state (verified 2026-11-16):** `scripts/drift-check.py` (`D08 T02 §7`) exempts `HARNESS_RUNTIME_PARENTS` (`.claude` and `.codex`) directory `st_mtime_ns` changes in `_entry_state`, while `_watched_paths` adds parent directories of declared generator `outputs` (`build/milestones.json`), pulling `build` into the watched snapshot even though `ignored_dirs` ignores `build`. Whenever `scripts/campaign_guard.py heartbeat-still` atomically replaces `build/claude-campaign-state.json` during a concurrent `check-all.py` / `drift-check.py --write-site-receipt` run, `build`'s `st_mtime_ns` changes mid-check and fails `documentation site tests` / `research/milestones.py --check`.
<!-- claim: exists scripts/drift-check.py -->
<!-- claim: exists tests/test_drift.py -->
<!-- claim: count "HARNESS_RUNTIME_PARENTS" scripts/drift-check.py = 2 -->
<!-- claim: count "def _watched_paths" scripts/drift-check.py = 1 -->

## Inputs

- [`scripts/drift-check.py`](../../scripts/drift-check.py) -- `HARNESS_RUNTIME`, `HARNESS_RUNTIME_PARENTS`, `_entry_state`, `_watched_paths`, and `generators` read-only snapshot comparison
- [`tests/test_drift.py`](../../tests/test_drift.py) -- `test_harness_runtime_state_is_not_a_generator_write` and generator read-only regression tests
- [`scripts/campaign_guard.py`](../../scripts/campaign_guard.py) -- atomic updates to `build/claude-campaign-state.json` and `build/claude-campaign-guard.json`
- [`todo/08-docs/TODO-02-drift-gates.md`](./TODO-02-drift-gates.md) -- `D08 T02 §7` owning the `drift-check.py` read-only generator and built-site receipt contract

## Outcome

- `scripts/drift-check.py` ignores directory `st_mtime_ns` drift on ignored campaign-runtime container directories (or excludes ignored runtime state such as `build/claude-campaign-*` / `build` `st_mtime_ns` from triggering `--check modified build`) while still failing whenever a generator adds, deletes, or modifies any declared output file under `build/` (such as `build/milestones.json`) or mutates any tracked/unignored file in `--check` mode.
- `tests/test_drift.py` proves both that concurrent writes to `build/claude-campaign-state.json` during a generator `--check` (with `outputs: ["build/declared.json"]`) pass `drift.main`, and that mutating `build/declared.json` during `--check` still fails exit 1.

**Adjacency:** all=not-applicable (repository `scripts/drift-check.py` read-only snapshot boundary and its unit test; no codec, CLI, library, converter, or website runtime behavior)

**Adjacency rationale:** Only the `scripts/drift-check.py` snapshot/runtime-exclusion boundary and its `tests/test_drift.py` regression test change; no shipped binary, format, or user data model is touched.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Exempt ignored build-dir mtime drift during read-only generator checks | D08 T02 §7 |  [ ]   |

---

## 1. Exempt Ignored Build-Dir Mtime Drift During Read-Only Generator Checks

**Origin:** discovered run=0c72d521c489 2026-11-16 -- scripts/drift-check.py _watched_paths false-positive build directory mtime race during concurrent campaign_guard.py heartbeat writes

-> SOURCE: D10-T01-S6-DRIFT-BUILD-MTIME

Admission Clause 1 (defect in shipped `D08 T02 §7` work): during `run=0c72d521c489`, invoking `scripts/campaign_guard.py heartbeat-still` while `scripts/check-all.py` executed `tests/test_docs.py` (`drift-check.py --write-site-receipt`) atomically replaced `build/claude-campaign-state.json`, bumping `build`'s directory `st_mtime_ns` between `before_snap` and `after_snap` inside `_watched_paths` (`Path('build/milestones.json').parents` re-adds `'build'` even though `'build'` is in `ignored_dirs`) and causing `drift: generator: research/milestones.py: --check modified build; check mode must be read-only`. Fixing this at the owning `scripts/drift-check.py` snapshot layer preserves strict read-only detection for declared outputs under `build/` (like `build/milestones.json`) without false-failing on concurrent campaign runtime writes in `build/`.

- [ ] Inspect `HARNESS_RUNTIME`, `HARNESS_RUNTIME_PARENTS`, `_entry_state`, and `_watched_paths` in `scripts/drift-check.py` (`lines 1740-1809`) to mask transient `build/` campaign runtime updates (including `build/claude-campaign-*.json` and `build`'s `st_mtime_ns`, while still checking declared generator `outputs`/`cache_outputs` bytes and mode/owner plus unexpected children outside the ignored set) and ensure `_copy_checkout_to_stage` / `--write-site-receipt` stays unaffected. Done when: `python3 scripts/drift-check.py` ignores concurrent atomic replacement of `build/claude-campaign-state.json` during `--check` while still failing when a declared `outputs` file under `build/` (or any unignored directory/file) is modified in `--check` mode. Cheaper substitute: removing `build/` outputs from `_watched_paths` or skipping generator read-only snapshot checks altogether.
- [ ] Add positive and negative regressions right beside `test_harness_runtime_state_is_not_a_generator_write` in `tests/test_drift.py` demonstrating (a) concurrent atomic write/replace of `build/claude-campaign-state.json` during a generator `--check` with `outputs: ['build/declared.json']` exits 0 without reporting `--check modified build`, and (b) mutating `build/declared.json` during `--check` still exits 1 with `check mode must be read-only`. Done when: `python3 -m unittest tests/test_drift.py -v` passes with the new cases red against the unpatched snapshot logic.
- [ ] Commit: `"docs: ignore build campaign-state mtime churn in drift-check"`

**Test checkpoint:** `python3 -m unittest tests/test_drift.py -v` and `python3 scripts/drift-check.py` exit 0, and `bosun run -- python3 scripts/check-all.py --all-targets` exits 0 (`32/32 passed`) even alongside live `campaign_guard.py heartbeat-still` updates.

## Verification

- [ ] `python3 -m unittest tests/test_drift.py -v` exits 0
- [ ] `python3 scripts/drift-check.py` exits 0
- [ ] `python3 scripts/check-all.py --all-targets` exits 0
- [ ] `python3 scripts/todo-graph.py validate` clean
