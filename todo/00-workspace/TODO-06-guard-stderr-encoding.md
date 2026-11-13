---
schema_version: 1
id: guard-stderr-encoding
domain: 00-workspace
status: draft
title: "TODO-06 -- Campaign Guard Legacy-Encoding Stderr Self-Test"
depends_on: []
track: W1
---

# TODO-06 -- Campaign Guard Legacy-Encoding Stderr Self-Test

> **Goal:** Prove directly in the campaign guard test suite that `sys.stderr` as well as `sys.stdout` survives non-cp1252 UTF-8 characters when launched under a legacy narrow encoding (`PYTHONIOENCODING=cp1252`).

> [!IMPORTANT]
> **Current state (verified 2026-11-13):** `scripts/campaign_guard.py` reconfigures both `sys.stdout` and `sys.stderr` to UTF-8 in one loop, and `self_test()` in `scripts/campaign_guard.py` (`the-guard-prints-on-a-legacy-console-encoding`) exercises `PYTHONIOENCODING=cp1252` on `stdout`, but no `self-test` case emits a non-cp1252 diagnostic on `stderr` under `PYTHONIOENCODING=cp1252` and verifies the UTF-8 bytes intact.
<!-- claim: exists scripts/campaign_guard.py -->
<!-- claim: count "the-guard-prints-on-a-legacy-console-encoding" scripts/campaign_guard.py = 1 -->

## Inputs

- [`scripts/campaign_guard.py`](../../scripts/campaign_guard.py) -- UTF-8 stream reconfiguration, `stderr` error reporting, and `self_test()` (`the-guard-prints-on-a-legacy-console-encoding`)
- -> XREF: D09 T01 §4 -- release backlog review for `rasterfold-v1.0.0` that promoted `B-001`

## Outcome

- `python3 scripts/campaign_guard.py self-test` proves in a separate case that `scripts/campaign_guard.py` writes non-cp1252 UTF-8 characters (such as `U+0100` `\u0100`) to `stderr` without `UnicodeEncodeError` or replacement corruption when `PYTHONIOENCODING=cp1252` is set.

**Adjacency:** all=not-applicable (runner self-test coverage in `scripts/campaign_guard.py`; no codec, CLI, library, or site behavior)

**Adjacency rationale:** Only `self_test()` in `scripts/campaign_guard.py` changes; no user-facing artifact or codec output is affected.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | Separate stderr assertion for legacy-encoding stream setup | D00 T01 §7, D05 T01 §10 |  [ ]   |

---

## 1. Separate Stderr Assertion for Legacy-Encoding Stream Setup

**Origin:** discovered run=36a212a58ece 2026-11-13 -- promoted from `B-001` during `D09 T01 §4` release backlog review

-> SOURCE: D00-T01-S7-PR2

`D00 T01 §7` reconfigured `sys.stdout` and `sys.stderr` to UTF-8 together, while its `self_test()` case (`the-guard-prints-on-a-legacy-console-encoding`) asserted only `stdout`. If stream setup later splits per stream or an error path writes before `stderr` is reconfigured, a `stdout`-only case stays green while `stderr` crashes on characters outside `cp1252`.

- [ ] Add a dedicated subprocess case in `self_test()` in `scripts/campaign_guard.py` right beside `the-guard-prints-on-a-legacy-console-encoding` that invokes `scripts/campaign_guard.py` with `PYTHONIOENCODING=cp1252` on a failure/refusal path whose `stderr` message carries `U+0100` (`\u0100`, outside `cp1252`, e.g. a missing `--run-file docs/phase-runs/2099-01-01-\u0100-missing.md`), asserting nonzero exit with no `UnicodeEncodeError` traceback and the exact UTF-8 character intact on `stderr` (and proving red against a mutant without `sys.stderr.reconfigure`). Done when: `python3 scripts/campaign_guard.py self-test` passes on Linux and Windows (`scripts/windows.py`). Cheaper substitute: checking `sys.stderr.encoding` in-process without running a `PYTHONIOENCODING=cp1252` child process.
- [ ] Commit: `"test: cover campaign guard stderr UTF-8 output under cp1252"`

**Test checkpoint:** `python3 scripts/campaign_guard.py self-test` and `python3 scripts/windows.py scripts/campaign_guard.py self-test` exit 0 with `0 failed` including the new `stderr` case.

## Verification

- [ ] `python3 scripts/campaign_guard.py self-test` exits 0
- [ ] `python3 scripts/check-all.py` exits 0
- [ ] `python3 scripts/todo-graph.py validate` clean
