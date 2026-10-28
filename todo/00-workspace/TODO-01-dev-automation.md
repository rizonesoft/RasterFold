---
schema_version: 1
id: dev-automation
domain: 00-workspace
status: active
title: "TODO-01 -- Dev-Automation Wiring"
depends_on: []
track: W1
---

# TODO-01 -- Dev-Automation Wiring

> **Goal:** The plan polices itself on every clone and every push: the ported TODO system, skills, hooks, and gates are committed with the hooks executable, the commit hook refuses a broken staged tree, the first push comes back green from both workflows, a static analysis gate holds the C sources to a baseline that only shrinks, the review panel is proven end to end before the first stamp depends on it, and the claims ratchet sits at what the tree measures.

> [!IMPORTANT]
> **Current state (verified 2026-10-25):** The port, four-target build, and sole-writer Antigravity (`agy`) + GPT-6 Astra (`scripts/review-dispatch.py`) review architecture are in place. This WSL checkout has `core.hooksPath=tools/githooks` and authenticated GitHub CLI; Windows/Linux build and plan workflows have succeeded. Clone provisioning remains required. `.clang-tidy`, `scripts/tidy.py`, and `todo/.tidy-baseline` (`count: 20`) gate static analysis across Windows/POSIX and x64/ARM64 compilation databases; `COVERAGE_FLOOR` is 32 after `D00 T01 §5` measured 32/32 covered `Current state` blocks across all 32 TODO files. Local/remote migration evidence is in `docs/wsl-migration-audit.md`.
<!-- claim: exists tools/githooks/pre-commit -->
<!-- claim: exists tools/githooks/commit-msg -->
<!-- claim: exists tools/provision.ps1 -->
<!-- claim: exists .conclave/reviewers.toml -->
<!-- claim: exists .clang-tidy -->
<!-- claim: exists scripts/tidy.py -->
<!-- claim: exists todo/.tidy-baseline -->
<!-- claim: count "^COVERAGE_FLOOR = 32$" scripts/todo-claims.py = 1 -->

## Inputs

- [`scripts/plan-audit.py`](../../scripts/plan-audit.py) -- prerequisite order and release requirement coverage.

- [`tools/githooks/pre-commit`](../../tools/githooks/pre-commit) -- the hook §1 wires in; it validates the staged tree in a temp checkout
- [`tools/provision.ps1`](../../tools/provision.ps1) -- sets `core.hooksPath` (§1) and verifies the toolchain
- [`.github/workflows/plan.yml`](../../.github/workflows/plan.yml), [`.github/workflows/build.yml`](../../.github/workflows/build.yml), and [`scripts/ci_status.py`](../../scripts/ci_status.py) -- the workflows §2 reads back
- [`scripts/check-all.ps1`](../../scripts/check-all.ps1) -- the gate runner §3 adds the static analysis gate to
- [`.conclave/reviewers.toml`](../../.conclave/reviewers.toml) and [`scripts/review-dispatch.py`](../../scripts/review-dispatch.py) -- the reviewer pin, adaptive schedule, and dispatcher that §4 and §6 prove for Antigravity (`agy`) -> GPT-6 Astra (`astra`)
- [`scripts/todo-claims.py`](../../scripts/todo-claims.py) -- `COVERAGE_FLOOR`, which §5 raises
- -> XREF: D99 T01 §2 -- the GitHub CLI login §2 needs before `ci_status.py` can read CI back
- -> XREF: D00 T04 §1 -- the local toolchain candidate whose stamp uses §6's `review-dispatch.py` route

## Outcome

- `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints mode `100755` for both, and a provisioned clone refuses a commit whose staged tree fails `validate`.
- The first push of `main` has a green `plan-gates` run and a green `build` run, quoted by URL in this file's stamp.
- `scripts/check-all.py` and its PowerShell wrapper run target-aware clang-tidy over Windows/POSIX and x64/ARM64 compilation databases; the gate fails on a warning count above `todo/.tidy-baseline`.
- Each review stage (`--kind plan`, `--kind completion`, and `--kind round`) on the `agy -> astra` route has run against a probe with its parsed evidence (`candidate`, `reviewer`, `model`, `effort`, `lenses`, `lines`, `digest`, and verdict) committed in §4 and its scratch record removed after read-back, while every real stamp commits its review record under `docs/reviews/`.
- `COVERAGE_FLOOR` equals the number of `Current state` blocks carrying a claim on the day §5 ships.

**Adjacency:** all=not-applicable (repository plumbing: gates, hooks, and CI with no user-facing file, option, or output)

**Adjacency rationale:** Everything in this file is developer tooling. It changes no byte the codec writes and no option a user passes.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Commit the port and wire the commit hook      | --         |  [x]   |
|   2   |   §2    | First push: CI green and read back            | §1, D99 T01 §2 |  [x]   |
|   3   |   §3    | The static analysis gate                      | §1         |  [x]   |
|   4   |   §4    | Prove the review panel end to end             | §1, §6     |  [x]   |
|   5   |   §5    | Raise the claims coverage floor               | §1         |  [x]   |
|   6   |   §6    | Claude review rounds through the reviewer pin | §1         |  [x]   |
| 7 | §7 | The campaign guard on a legacy console encoding | -- | [x] |
| 8 | §8 | A read-only independent reviewer beside a live run | -- | [x] |
| 9 | §9 | Windows Job Object process-tree cleanup in review-dispatch.py | §8 | [x] |
| 10 | §10 | OpenCode campaign Stop hook and heartbeat | §8 | [ ] |

---

## 1. Commit the Port and Wire the Commit Hook

> **Started:** 2026-10-24T20:16:25Z

The hook is the only gate that runs before a commit exists, so a tree that fails `validate` never reaches the remote. It must stay a POSIX shell script with LF endings, because Git for Windows runs hooks under its own `sh`, and it must be recorded executable, because Windows does not keep the file mode.

**Corrected 2026-10-24:** The port and hook wiring first landed in `97e5ca5` (`workspace: port the TODO system, skills, hooks, and gates from Isotone`), and WSL/Linux runs `python3 scripts/check-all.py` alongside `pwsh scripts/check-all.ps1`. This section verifies every hook, `.gitattributes`, `provision.py --verify`, and `plan-audit.py` requirement, runs the scratch-clone broken-tree refusal probe, and commits the section's verification record.

- [x] Run `python3 tools/provision.py` in WSL/Linux or `powershell -File tools/provision.ps1` on Windows. Done when: the host provisioner in verification mode (`--verify`) enforces Python 3.11+ (`sys.version_info >= (3, 11)`), exits zero and reports the local toolchain, hook, Python/Pillow and required host checks and `git config core.hooksPath` prints `tools/githooks`. Cheaper substitute: copying the hooks into `.git/hooks`, which no clone inherits.
- [x] Stage the ported tree (`todo/`, `scripts/`, `.agents/`, `.conclave/`, `tools/githooks/`, `tools/provision.ps1`, `toolchain.json`, `AGENTS.md`, `GEMINI.md`, `docs/`, `.github/`, `.gitattributes`) and record both hooks executable with `git update-index --chmod=+x tools/githooks/pre-commit tools/githooks/commit-msg` (shipped in `97e5ca5` and `c065b58`). Done when: `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints mode `100755` for both.
- [x] Confirm `.gitattributes` pins the hooks to LF. Done when: `git check-attr eol -- tools/githooks/pre-commit tools/githooks/commit-msg` prints `eol: lf` for both.
- [x] Confirm the hook refuses a broken staged tree and exits non-zero on `HUP`/`INT`/`TERM`: configure `git config core.hooksPath tools/githooks` (and verify read-back) in a scratch clone, stage a copy of a TODO file with its `Commit:` item removed, and run `git commit` in that scratch clone. Done when: `git commit` exits non-zero with `pre-commit: todo-graph validate is red on the staged tree`, `HEAD` in the scratch clone is unchanged, and the scratch clone is deleted.
- [x] Keep `scripts/plan-audit.py` wired into staged hooks, `scripts/check-all.py` and `.github/workflows/plan.yml`, running against the canonical graph and `docs/vision-map.json`. Done when: later prerequisites, disconnected release requirements, hook cancellation, and pre-3.11 Python interpreters fail (`python3 -m unittest discover -s tests -p 'test_plan_audit.py' -v` runs 17 tests and passes), while all product owners reach the release gate (`python3 scripts/plan-audit.py`).
- [x] Commit: `"workspace: verify the ported TODO system, hooks, and gates (D00 T01 §1)"` (the port first landed in `97e5ca5`)

**Test checkpoint:** `python3 scripts/check-all.py` (or `pwsh scripts/check-all.ps1`) exits 0 with every gate `PASS` at the commit; `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints `100755` for both; `git check-attr eol -- tools/githooks/pre-commit tools/githooks/commit-msg` prints `eol: lf`; the refused scratch commit's message is quoted.

> **Verified:** 2026-10-24 | §1 | 264158d8f4f8d74417d27a50bb288f8c0138958d
> **Review:** `docs/reviews/d00-t01-s1.json`
## 2. First Push: CI Green and Read Back

> **Started:** 2026-10-25T00:19:00Z

**Needs:** Network (downloads)

A workflow nobody has seen run is a claim, not a gate. The `plan-gates` workflow validates the plan and supported documentation on every main push; the `build` workflow bootstraps the pinned toolchain on a clean runner, builds, and runs the correctness suite. Both are read back with `scripts/ci_status.py`, so a red run is seen by the session that caused it.

**Corrected 2026-10-25:** Per the pre-build `--kind plan` review (`docs/reviews/00-workspace/D00-T01-s2.md`), the deliberate broken-TODO probe on scratch branch `probe/d00-t01-s2-red-plan` (`606c7f572af2a6666ab587f3f58b108daa4dfe6e`, triggered via `gh workflow run plan.yml --ref probe/d00-t01-s2-red-plan`) must record both the `scripts/ci_status.py --workflow plan.yml --branch probe/d00-t01-s2-red-plan` verdict (`plan.yml: red at 606c7f572af2 (https://github.com/rizonesoft/RasterFold/actions/runs/40146423134)`, exit 1) and the `Validate the TODO tree` step diagnostic (`FATAL todo/00-workspace/TODO-01-dev-automation.md:86: §2 has no '- [ ] Commit:' checklist item`, `32 todos, 227 sections -- 1 fatal, 0 warning(s)`) proving the deliberate `no-commit-item` defect caused the failure before the branch is deleted (`git push origin --delete probe/d00-t01-s2-red-plan`).

- [x] Push `main` and read both workflows back with `python3 scripts/ci_status.py`. Done when: both `build.yml` and `plan.yml` print a `green` verdict with the run URL (`build.yml: green at 5f46c6a38ffa (https://github.com/rizonesoft/RasterFold/actions/runs/40146055234)` and `plan.yml: green at 5f46c6a38ffa (https://github.com/rizonesoft/RasterFold/actions/runs/40146055157)` on the baseline push, and `build.yml: green at 6ad7aafe0ad9 (https://github.com/rizonesoft/RasterFold/actions/runs/40146688580)` and `plan.yml: green at 6ad7aafe0ad9 (https://github.com/rizonesoft/RasterFold/actions/runs/40146688570)` with full `headSha` `6ad7aafe0ad9ffdf9a31584429b7dd8a218a1d10` on this section's pushed candidate). Cheaper substitute: opening the Actions page by hand, which leaves no record.
- [x] Record both run URLs and their conclusions in this section's stamp, and prove a deliberately broken TODO pushed to scratch branch `probe/d00-t01-s2-red-plan` (`606c7f572af2a6666ab587f3f58b108daa4dfe6e`) makes `plan-gates` fail at `Validate the TODO tree` (`plan.yml: red at 606c7f572af2 (https://github.com/rizonesoft/RasterFold/actions/runs/40146423134)`, `failed job: plan-gates (failure) https://github.com/rizonesoft/RasterFold/actions/runs/40146423134/job/121028631004`, step diagnostic `FATAL todo/00-workspace/TODO-01-dev-automation.md:86: §2 has no '- [ ] Commit:' checklist item`, exit 1) before deleting `probe/d00-t01-s2-red-plan`. Done when: the stamp quotes both green lines as `ci_status.py` printed them and the red probe run URL and `no-commit-item` diagnostic are recorded.
- [x] Commit: `"workspace: read the first CI runs back"`

**Test checkpoint:** `python3 scripts/ci_status.py` exits 0 for both workflows on the pushed commit (`build.yml: green at 6ad7aafe0ad9 (https://github.com/rizonesoft/RasterFold/actions/runs/40146688580)`, `plan.yml: green at 6ad7aafe0ad9 (https://github.com/rizonesoft/RasterFold/actions/runs/40146688570)`, `headSha` `6ad7aafe0ad9ffdf9a31584429b7dd8a218a1d10`, after `build.yml: green at 5f46c6a38ffa (https://github.com/rizonesoft/RasterFold/actions/runs/40146055234)` and `plan.yml: green at 5f46c6a38ffa (https://github.com/rizonesoft/RasterFold/actions/runs/40146055157)`); a deliberately broken TODO pushed to scratch branch `probe/d00-t01-s2-red-plan` (`606c7f572af2a6666ab587f3f58b108daa4dfe6e`) makes `plan-gates` fail (`python3 scripts/ci_status.py --workflow plan.yml --branch probe/d00-t01-s2-red-plan` exits 1 with `plan.yml: red at 606c7f572af2 (https://github.com/rizonesoft/RasterFold/actions/runs/40146423134)`, `failed job: plan-gates (failure) https://github.com/rizonesoft/RasterFold/actions/runs/40146423134/job/121028631004`, `Validate the TODO tree` step `FATAL todo/00-workspace/TODO-01-dev-automation.md:86: §2 has no '- [ ] Commit:' checklist item`), and the red run's URL is quoted before the branch is deleted.

> **Verified:** 2026-10-25 | §2 | 0d5a57cf8a0548280ccadcb061eb01fe37bc2a5c
> **Review:** `docs/reviews/d00-t01-s2.json`
## 3. The Static Analysis Gate

> **Started:** 2026-10-24T20:37:00Z

The build already fails on any compiler warning (`-Werror` in `scripts/build.py`). Static analysis finds what the compiler does not: a read past a buffer on one path, an unchecked size, a leak. The gate is a ratchet: the count found on the day it lands is the baseline, and it only shrinks.

- [x] Add `.clang-tidy` with `bugprone-*`, `clang-analyzer-*`, `performance-*`, and `cert-*` checks, minus the checks that report style rather than defects (`bugprone-easily-swappable-parameters`, `readability-magic-numbers`, and any other noise the first run shows, each named with its reason in a comment). Done when: a `scripts/tidy.py` driver uses each target's compilation database and pinned clang-tidy (`linux-x64`, `linux-x64-portable`, `windows-x64`, `windows-x64-portable`, `android-arm64`, `android-x64` on Linux/WSL; `windows-x64`, `windows-x64-portable`, `posix-x64`, `posix-arm64` on Windows, printing its host coverage summary on each run), covering Windows/POSIX, x64 SIMD and ARM64 portable paths on both hosts. Cheaper substitute: enabling every check and baselining thousands of style findings.
- [x] Record the warning count in `todo/.tidy-baseline` as `count: 20`, counting unique `(file, line, column, check)` findings, enforce a shrinking ratchet (`scripts/tidy.py` fails on `measured > baseline`, `measured < baseline` without `--ratchet`, or `baseline > HEAD:todo/.tidy-baseline`; `scripts/tidy.py --ratchet` shrinks `todo/.tidy-baseline` when warnings drop), pin `.clang-tidy` and `todo/.tidy-baseline` to LF in `.gitattributes`, and update this file's `Current state` `.clang-tidy` claim (`exists .clang-tidy`) when `.clang-tidy` lands. Done when: `todo/.tidy-baseline` exists, its count equals the run's (`count: 20`) on both Linux/WSL and Windows, and `python3 scripts/todo-claims.py` exits 0.
- [x] Add a `clang-tidy` gate to `scripts/check-all.py` (also used by `scripts/check-all.ps1`) that fails when the count differs from the baseline, when `clang-tidy` exits non-zero or emits `error:`, or when a compilation database or baseline file is missing or malformed (`TidyGateTests` in `tests/test_plan_audit.py` tests the `18 -> 15 -> 16` ratchet sequence, upward baseline edit refusal, cross-target deduplication, and analyzer/compdb/baseline failure paths), and renew `README.md`, `docs/building.md`, and `CONTRIBUTING.md`'s review receipts in `docs/drift.json` for the updated `scripts/build.py` and `scripts/check-all.py`. Done when: the gate passes on the tree, and a probe line added to `src/mem.c` that reads an uninitialized variable makes it fail (`src/mem.c:39:15: warning: Assigned value is uninitialized [clang-analyzer-core.uninitialized.Assign]`, `tidy: clang-tidy found 21 unique warning(s), exceeding baseline 20`); the probe is reverted.
- [x] Commit: `"workspace: gate the C sources on clang-tidy against a shrinking baseline"`

**Test checkpoint:** `pwsh scripts/check-all.ps1` shows the `clang-tidy` gate `PASS`; the probe run's `FAIL` line and its finding are quoted.

> **Verified:** 2026-10-24 | §3 | 6ddd5ddf7d4a5d8312ee28de460d9b6726b7ade4
> **Review:** `docs/reviews/d00-t01-s3.json`
## 4. Prove the Review Panel End to End

> **Started:** 2026-10-24T23:44:00Z

**Corrected 2026-10-24:** With the operator decision to consolidate on Antigravity (`agy`) as the sole writer and GPT-6 Astra (`gpt-6-astra` via `codex`) as the sole reviewer through `.conclave/reviewers.toml` and `scripts/review-dispatch.py`, this section proves the `agy -> astra` route and the 3-stage adaptive schedule (`--kind plan`, `--kind completion`, and `--kind round`) end to end instead of the retired `.conclave/panel.toml` and `scripts/panel_slots.py`.

- [x] Run `python3 scripts/review-dispatch.py --check-pin --json` and a live 3-stage probe (`--kind plan --candidate 264158d8f4f8d74417d27a50bb288f8c0138958d`, `--kind completion --candidate 264158d8f4f8d74417d27a50bb288f8c0138958d`, and `--kind round` naming `264158d8f4f8d74417d27a50bb288f8c0138958d` in the prompt) into a scratch record `build/reviews/probe/records/probe.md`, read it back with `python3 scripts/review-dispatch.py --list-rounds --records build/reviews/probe/records --json`, commit the parsed evidence below, and remove the scratch record. Done when: `--check-pin` exits 0, all three stages return valid verdicts from `astra` (`gpt-6-astra`), and `--list-rounds --records` parses all three sections with 0 unreadable files. Cheaper substitute: `codex --version`, which proves the CLI exists and nothing else.
- [x] Verify `python3 scripts/review-dispatch.py --list-rounds --json` and `python3 scripts/todo-runs.py --check` exit 0 with 0 unreadable files. Done when: both exit 0.
- [x] Verify `python3 -m unittest discover -s scripts/tests -v` (Linux) and `python3 scripts/windows.py` on `scripts/tests/test_review_dispatch.py`, `scripts/tests/test_automations.py`, and `scripts/tests/test_intelligence.py` (Windows) pass all cases. Done when: both Linux and Windows test runs exit 0.
- [x] Commit: `"workspace: prove the review dispatcher against a probe"`

**Test checkpoint:** `python3 scripts/review-dispatch.py --check-pin --json`, `python3 scripts/review-dispatch.py --list-rounds --json`, `python3 scripts/todo-runs.py --check`, `python3 -m unittest discover -s scripts/tests -v`, and the Windows test equivalents exit 0; the 3-stage live probe's candidate, reviewer, model, effort, lenses, line count, digest, and verdict summary is quoted and committed here after scratch-record read-back.

**Checkpoint output (2026-10-24):** `python3 scripts/review-dispatch.py --check-pin --json` exited 0 (`"ok": true`, `"routes": {"agy": ["astra"]}`, `"reviewers": {"astra": {"cli": "codex", "model": "gpt-6-astra", "effort": "medium"}}`, `"severity_stop_after": 0`, `"clis": {"codex": true}`). The 3-stage live probe against `D00 T01 §1` commit `264158d8f4f8d74417d27a50bb288f8c0138958d` into `build/reviews/probe/records/probe.md` ran all three passes and was read back cleanly by `python3 scripts/review-dispatch.py --list-rounds --records build/reviews/probe/records --json` (`"unreadable": []`) before `build/reviews/probe/records` was removed:
1. `--kind plan --candidate 264158d8f4f8d74417d27a50bb288f8c0138958d`: `run=2026-10-24T23:47:28Z`, `reviewer=astra`, `model=gpt-6-astra`, `cli=codex`, `writer=agy`, `effort=medium`, `lenses=["plan"]`, `lines=9`, `digest=3573787d5513da14`, verdict `HIGHEST: P2` / `PLAN: gaps` / `OVERALL: needs-attention`.
2. `--kind completion --candidate 264158d8f4f8d74417d27a50bb288f8c0138958d`: `run=2026-10-24T23:48:04Z`, `reviewer=astra`, `model=gpt-6-astra`, `cli=codex`, `writer=agy`, `effort=medium`, `lenses=null`, `lines=7`, `digest=ef8ca9140b6a3493`, verdict `COMPLETION: complete`.
3. `--kind round` (Round 1 on `264158d8f4f8d74417d27a50bb288f8c0138958d`): `run=2026-10-24T23:49:15Z`, `reviewer=astra`, `model=gpt-6-astra`, `cli=codex`, `writer=agy`, `effort=high`, `lenses=["adversarial", "integration"]`, `lines=12`, `digest=74adf3662c008596`, verdict `HIGHEST: none` / `OVERALL: approve`.
`python3 scripts/review-dispatch.py --list-rounds --json` exited 0 (`"unreadable": []`); `python3 scripts/todo-runs.py --check` exited 0 (`2 runs, 5 rounds: all resolve, all covered, counts agree`); `python3 -m unittest discover -s scripts/tests -v` exited 0 (`Ran 104 tests in 33.682s`, `OK (skipped=1)`); and Windows test runs via `scripts/windows.py` (`test_review_dispatch.py` `Ran 69 tests, OK (skipped=1)`, `test_automations.py` `Ran 30 tests in 1.657s, OK (skipped=3)`, and `test_intelligence.py` `Ran 5 tests in 1.790s, OK`) all exited 0.

> **Verified:** 2026-10-25 | §4 | 63af0bcdc211d4e5567d215ed5ac9c2ac70bfdfc
> **Review:** `docs/reviews/d00-t01-s4.json`
## 5. Raise the Claims Coverage Floor

> **Started:** 2026-10-24T23:59:00Z

`COVERAGE_FLOOR` is the number of `Current state` blocks that must carry a claim. On 2026-09-30 it was set to 31 (`31/31`) because `CURRENT_STATE_RE` in `scripts/todo-claims.py` (`r"\*\*Current state\s*\(verified\s+(\d{4}-\d{2}-\d{2})\)"`) required an immediate closing `)` after the date and silently skipped `todo/02-speed/TODO-01-compiler-options.md:16` (`**Current state (verified 2026-09-30; timings below remain the 2026-09-29 baseline):**`), even though that block carries 4 claims. Allowing optional `; ...` notes inside `CURRENT_STATE_RE`, stripping inline code spans before matching `CURRENT_STATE_RE`, and passing `-c safe.directory=*` to `git` in `_ignored` and `_git_last_change` (so Windows `git` runs cleanly on the WSL UNC checkout) brings the measured count to `32/32` across all 32 TODO files, raising `COVERAGE_FLOOR` from `31` to `32`.

- [x] Teach `CURRENT_STATE_RE` in `scripts/todo-claims.py` to match `**Current state (verified YYYY-MM-DD; ...):**` blocks while ignoring inline-code quotations (with a self-test case in `_self_test()`), run `python3 scripts/todo-claims.py --coverage`, and set `COVERAGE_FLOOR` in `scripts/todo-claims.py` to `32` (`32/32` covered blocks). Done when: `python3 scripts/todo-claims.py --coverage` and `python3 scripts/todo-claims.py --self-test` exit 0 on Linux and Windows, and removing the claims from a block makes `--coverage` exit 1 (`coverage fell to 31, below the recorded floor of 32`). Cheaper substitute: raising the floor by an arbitrary number or leaving the 32nd block uncounted.
- [x] Update this file's `Current state` prose and claim on `COVERAGE_FLOOR` (`^COVERAGE_FLOOR = 32$`) to the new value. Done when: `python3 scripts/todo-claims.py` exits 0.
- [x] Commit: `"workspace: raise the claims coverage floor to the measured count"`

**Test checkpoint:** `python3 scripts/todo-claims.py --coverage` and `python3 scripts/todo-claims.py --self-test` exit 0 at the new floor (`32/32`) and `--coverage` exits 1 with one block's claims removed (quoted, then restored).

**Checkpoint output (2026-10-25):** `python3 scripts/todo-claims.py` exited 0 (`todo-claims: 75 claim(s) -- 75 hold, 0 stale, 0 malformed; coverage 32/32, floor 32`); `python3 scripts/todo-claims.py --coverage` exited 0 (`todo-claims coverage: 32/32 Current state block(s) carry a claim`); `python3 scripts/todo-claims.py --self-test` and `python3 scripts/windows.py scripts/todo-claims.py --self-test < /dev/null` both exited 0 (`todo-claims self-test: 26 cases, 0 failed`); `python3 scripts/windows.py scripts/todo-claims.py --coverage --quiet < /dev/null` exited 0 (`todo-claims coverage: 32/32, floor 32`). Temporarily removing the claims from `todo/02-speed/TODO-01-compiler-options.md` made `python3 scripts/todo-claims.py --coverage --quiet` exit `1` with `FLOOR     coverage fell to 31, below the recorded floor of 32. The floor ratchets: raise it deliberately, never lower it to pass.` (`todo-claims coverage: 31/32, floor 32`); restoring `todo/02-speed/TODO-01-compiler-options.md` returned `--coverage --quiet` to exit `0` (`todo-claims coverage: 32/32, floor 32`).

> **Verified:** 2026-10-25 | §5 | 55ab7b6cc9ddc6ea0337dc0b63ddf3d657436bc1
> **Review:** `docs/reviews/d00-t01-s5.json`
## 6. Claude Review Rounds Through the Reviewer Pin

> **Started:** 2026-10-24T21:13:30Z

**Operator decision 2026-10-24:** Consolidate on Antigravity (`agy`, supporting both Claude and Gemini writer models) as the sole writer in RasterFold and GPT-6 Astra (`gpt-6-astra` via `codex`) as the sole independent reviewer in `.conclave/reviewers.toml` (`[routes] agy = ["astra"]`, no fallback), with Recto's 3-stage adaptive review schedule (`--kind plan` pre-build at `medium`, `--kind completion` post-build at `medium`, and adaptive code review rounds `high -> medium -> low -> low ...` ending the review loop on the first round with no Critical (`P1`) or Important (`P2`) finding (`HIGHEST: none` or `HIGHEST: P3`, `severity_stop_after = 0` / `SEVERITY_CUTOFF_ROUND = 0`, removing the round-4 wait) and escalating once to `medium` `adversarial` after a `low` round reports `HIGHEST: P1`). Retire the Claude CLI and Codex CLI writer harnesses (`.claude/`, `.codex/`, `CLAUDE.md`, `CODEX.md`) and legacy panel scripts (`.conclave/panel.toml`, `scripts/panel_slots.py`, `scripts/review_prompt.py`, `scripts/agent_routing.py`).

- [x] Configure `.conclave/reviewers.toml` and `scripts/review-dispatch.py` for `WRITERS = ("agy",)`, routing `agy = ["astra"]` (`codex`, `gpt-6-astra`, `openai`) with no fallback and the full `[schedule]` table (`plan`, `completion`, `rounds` at `high -> medium -> low`, `rotation` at `low`, `escalation` at `medium`, and `severity_stop_after = 0` so any round with `HIGHEST: P3` sets `severity_stop = true`). Done when: `python3 scripts/review-dispatch.py --check-pin --json` exits 0 and `python3 -m unittest discover -s scripts/tests -v` passes all tests.
- [x] Add `docs/plan-review-prompt.md`, `docs/completion-review-prompt.md`, and the multi-lens `docs/review-round-prompt.md`, and teach `scripts/todo-graph.py` (`stamp`, `unstamp`, `query rounds`, `resolve` `surface`, `SEVERITY_CUTOFF_ROUND = 0`) and `scripts/todo-validate.py` (`validate_section_receipt`) to verify and stamp `review-dispatch.py` records (including `stopped-low-severity` on any round `>= 1` with `HIGHEST: P3`) while preserving legacy stamp compatibility. Done when: `python3 scripts/todo-graph.py self-test` and `python3 scripts/todo-graph.py validate` exit 0.
- [x] Retire `.claude/`, `.codex/`, `CLAUDE.md`, `CODEX.md`, `.conclave/panel.toml`, `scripts/panel_slots.py`, `scripts/review_prompt.py`, and `scripts/agent_routing.py`, and consolidate all project skills under `.agents/skills/` and the `Stop` hook in `.agents/hooks.json`. Done when: `python3 scripts/campaign_guard.py --self-test` exits 0 against `.agents/hooks.json` and `.agents/skills/`.
- [x] Normalize legacy `## Plan review` / `## Round` headings in `docs/reviews/` so `python3 scripts/review-dispatch.py --list-rounds` reports 0 unreadable files. Done when: `python3 scripts/review-dispatch.py --list-rounds --json` reports `"unreadable": []` and `python3 scripts/todo-graph.py validate` exits 0.
- [x] Update `AGENTS.md`, `GEMINI.md`, `README.md`, `CONTRIBUTING.md`, `docs/README.md`, `docs/agent-skills-and-rules-structure.md`, `scripts/check-all.py`, and `.github/workflows/plan.yml` for the sole-writer `agy` + `astra` architecture. Done when: `python3 scripts/drift-check.py` exits 0.
- [x] Commit: `"todo: start D00 T01 §6 and record first-round P3 severity-stop rule"` (the initial sole-writer consolidation landed in `c065b58` and the first-round `P3` stop rule and route/footer hardening in `5446ac9`..`d47007b`)

**Test checkpoint:** `python3 scripts/check-all.py --quick`, `python3 scripts/review-dispatch.py --check-pin --json`, `python3 scripts/review-dispatch.py --list-rounds --json`, and `python3 -m unittest discover -s scripts/tests -v` exit 0 (quoted).

> **Verified:** 2026-10-24 | §6 | d47007b1d303228f07ad03289f5a2db106a99373
> **Review:** `docs/reviews/d00-t01-s6.json`
## 7. The Campaign Guard on a Legacy Console Encoding

**Origin:** discovered run=9bdc68a3a5dc 2026-10-21 -- the SHIP push of `D99 T01 §3` read back red

> **Started:** 2026-10-21T06:25:47Z

Admission: a defect in shipped work. The Python Stop hook landed in `a9a5af7` to `4fd93a0` (2026-10-21, before this section existed) and the first push carrying it turned `plan-gates` red on `windows-2025` (run `39554469979`): `block-open-run-naming-next-row` saw `�` where the next row's `§3` belongs, and printing that FAIL line then raised `UnicodeEncodeError: 'charmap' codec can't encode character '�'`, so the job died with a traceback instead of a count. Two causes, both in `scripts/campaign_guard.py`. `ready_rows` decodes the `query ready` child's stdout as UTF-8 but never asks the child for UTF-8, so a child that writes in the console code page (cp1252 on a stock Windows runner) garbles every non-ASCII row name the hook shows the session; the real `scripts/todo-graph.py` reconfigures its own streams, so the defect hides until any other producer answers. And the guard's own streams keep the console code page, so any line it prints with a character outside cp1252 crashes the command that prints it. The local Windows host passed (`140 cases, 0 failed`) because it runs in UTF-8.

- [x] Make `ready_rows` run its child with `PYTHONIOENCODING=utf-8` in the child's environment, matching the UTF-8 it decodes. Done when: a self-test case runs the hook with `PYTHONIOENCODING=cp1252` against a fake graph that does not reconfigure its streams and finds `§3` in the block reason; the case fails before the fix.
- [x] Reconfigure the guard's own `stdout` and `stderr` to UTF-8 at import, the way `scripts/todo-graph.py` does. Done when: a self-test case runs a guard command that prints a character cp1252 cannot encode under `PYTHONIOENCODING=cp1252` and it exits 0 with the character intact; the case fails before the fix.
- [x] Commit: `"guard: decode the next row and print on a legacy console encoding"`

**Test checkpoint:** `python3 scripts/campaign_guard.py --self-test` exits 0 on Linux and through `python3 scripts/windows.py scripts/campaign_guard.py --self-test` on Windows, each quoting its `cases, 0 failed` line; both new cases quoted failing against the pre-fix module; `ci-wait` reads `plan-gates` green on the pushed repair, its `campaign-guard (windows-2025)` job included.

> **Verified:** 2026-10-21 | §7 | campaign_guard self-test 145 cases, 0 failed (Linux) · 142 cases, 0 failed (native Windows) · both new cases red against their mutants (145 cases, 1 failed each) · plan-gates green on the repair push (run 39556229372)
> **Review:** 3 rounds (Full), candidate `c4ae79b0e75e`(round 1) `19327d468ff9`(round 2) `19327d468ff9`(round 3) -- round 3 signoff: `adversarial` approve · `consistency` approve · `integration` approve · `record` approve; round 1 `consistency` needs-attention (1), fixed in round 2's candidate; the independent `codex review` of round 1's candidate found no actionable regressions. Raw findings: docs/reviews/00-workspace/D00-T01-s7.md. Attestation: docs/reviews/00-workspace/D00-T01-s7.attest.json
> **Plan review:** gpt high, 3 findings rejected, one to backlog B-001 (run 20261021-D00-T01-S7-gpt)
> **CRUD:** not applicable (the guard writes no codec file; its write path is the guard and state files, exercised by the self-test's acquire cases)
> **Implementer:** Claude (claude-opus-5-5)
> **Duration:** 2026-10-21T06:25:47Z to 2026-10-21T07:05:00Z

## 8. A Read-Only Independent Reviewer Beside a Live Run

**Origin:** discovered run=9bdc68a3a5dc 2026-10-21 -- the independent review of `D99 T01 §3` was refused

> **Started:** 2026-10-24T19:08:32Z

**Corrected 2026-10-24:** The 2026-10-24 consolidation onto Antigravity (`agy`) as the sole writer retired `.codex/hooks/adapter.py` and `scripts/panel_slots.py`. `scripts/review-dispatch.py` always invokes `codex exec ... --sandbox read-only` so GPT-6 Astra runs in a read-only sandbox beside a live `agy` campaign without writer-hook contention, and `--list-rounds` exits 1 when any findings file is unreadable.

- [x] Make `scripts/review-dispatch.py` run `codex exec` with adjacent `--sandbox read-only` for every review stage (`plan`, `completion`, and `round`). Done when: `python3 -m unittest discover -s scripts/tests -v` asserts the adjacent `["--sandbox", "read-only"]` argv for all three stages (`test_all_three_review_stages_pass_adjacent_read_only_sandbox_argv`, `test_sol_reviews_agy_work_read_only`, `test_the_plan_review_is_one_pass_recorded_with_its_lens`, `test_the_completion_review_takes_its_scheduled_effort_and_no_lenses`) and passes.
- [x] Retire the `.codex/hooks/adapter.py` PreToolUse writer block so `codex exec` runs solely as the read-only reviewer beside a live Antigravity campaign, and prove coexistence with a live driven probe while the campaign guard is active. Done when: `python3 scripts/campaign_guard.py --self-test` and `python3 -m unittest discover -s scripts/tests -v` pass, and a live `scripts/review-dispatch.py` probe with an active campaign guard reads the candidate, refuses a harmless attempted write inside `--sandbox read-only`, and leaves repository bytes, guard ownership, and the writer's `Stop` hook block unchanged.
- [x] Verify the reviewer pin and round listing beside the guard tooling, and make `scripts/review-dispatch.py --list-rounds` exit 1 when any findings file or findings subdirectory is unreadable. Done when: `python3 scripts/review-dispatch.py --check-pin --json` and `python3 scripts/review-dispatch.py --list-rounds --json` exit 0 with `"unreadable"` absent, and malformed-record or unsearchable-subdirectory fixtures make `--list-rounds --json` exit 1 (`test_a_malformed_or_unreadable_record_is_named_and_the_rest_still_listed`, `test_an_unsearchable_findings_subdirectory_is_reported_unreadable`).
- [x] Commit: `"review: run the independent reviewer read-only beside a live run (D00 T01 §8)"`

**Test checkpoint:** `python3 -m unittest discover -s scripts/tests -v`, `python3 scripts/campaign_guard.py --self-test`, `python3 scripts/review-dispatch.py --check-pin --json`, and `python3 scripts/review-dispatch.py --list-rounds --json` exit 0 (with `--list-rounds` exiting 1 on a malformed findings record); a live `scripts/review-dispatch.py` probe beside an active campaign guard returns a valid review from `astra`, proves `--sandbox read-only` refuses a write, and leaves guard ownership and the writer's `Stop` hook protection intact.

> **Verified:** 2026-10-24 | §8 | 2c37958e22415085911b516cdb5f1180807df723
> **Review:** `docs/reviews/d00-t01-s8.json`
## 9. Windows Job Object Process-Tree Cleanup in review-dispatch.py

**Origin:** discovered run=7c814ed0d16f 2026-10-24 -- completion review of `D00 T01 §8`

> **Started:** 2026-10-24T19:41:15Z

-> SOURCE: D00-T01-S8-COMPLETION-1

Admission: a defect in shipped work (`scripts/review-dispatch.py:309`). On Windows, `kill_tree` uses `taskkill /T /F /PID`, which reaches descendant helper processes only while the root reviewer process is still alive; if the reviewer exits before a child helper, the child can outlive the review and hold scratch files open (`test_a_reviewers_children_do_not_outlive_it` is skipped on Windows at `scripts/tests/test_review_dispatch.py:644`). Assigning the reviewer process to a Win32 Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (and terminating/closing the job in `kill_tree`) guarantees that every descendant process is terminated on Windows just as `os.killpg` does on POSIX.

**Corrected 2026-10-24:** `scripts/windows.py` takes a script path as its first argument (`python3 scripts/windows.py scripts/tests/test_review_dispatch.py -v`), not `-m unittest`. Furthermore, per the pre-build `--kind plan` review (`docs/reviews/00-workspace/D00-T01-s9.md`), Windows containment must attach before any user instruction in the reviewer process executes (`CREATE_SUSPENDED`, `AssignProcessToJobObject`, then `NtResumeProcess`), must refuse with `"could not start: ..."` and clean up the process/job if Job Object creation, configuration, assignment, or resume fails (never falling back to uncontained execution), and must test descendant cleanup across normal exit, timeout, output-flood, and containment-setup failure paths.

- [x] Bind the reviewer child process on Windows to a Win32 Job Object before it executes in `scripts/review-dispatch.py`: create the job (`CreateJobObjectW`), configure `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (`SetInformationJobObject`), launch the reviewer with `CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP`, assign it (`AssignProcessToJobObject`), and resume it (`ntdll.NtResumeProcess`), terminating and closing the job in `kill_tree` and refusing with `"could not start: ..."` (terminating any suspended process and closing handles) if containment setup fails. Done when: `kill_tree` terminates a child helper spawned before stdin is read on Windows without leaving the `WORKAROUND` limitation or any uncontained fallback.
- [x] Enable `test_a_reviewers_children_do_not_outlive_it` on Windows in `scripts/tests/test_review_dispatch.py` (spawning the child before reading stdin, and checking process liveness cleanly on both POSIX and Windows), and add regression coverage for descendant cleanup on timeout and output flood plus forced Job Object creation/configuration/assignment/resume failure. Done when: `@unittest.skipIf(os.name == "nt", ...)` is removed and all tests pass on both Linux (`python3 -m unittest discover -s scripts/tests -v`) and Windows (`python3 scripts/windows.py scripts/tests/test_review_dispatch.py -v < /dev/null`).
- [x] Commit: `"review: terminate Windows reviewer descendant trees with a Job Object (D00 T01 §9)"`

**Test checkpoint:** `python3 -m unittest discover -s scripts/tests -v` on Linux and `python3 scripts/windows.py scripts/tests/test_review_dispatch.py -v < /dev/null` on Windows exit 0 with `test_a_reviewers_children_do_not_outlive_it`, descendant-bearing timeout/flood cleanup tests, and Job Object failure-containment tests running and passing on both platforms.

> **Verified:** 2026-10-24 | §9 | 32bcc7acb05189ccd93d3517931df67c10d3848e
> **Review:** `docs/reviews/d00-t01-s9.json`
## 10. OpenCode Campaign Stop Hook and Heartbeat

Operator decision 2026-10-29: OpenCode (`opencode`, running an Anthropic model) writes in this repository, with its work reviewed by GPT-6 Astra (`opencode = ["astra"]` in `.conclave/reviewers.toml`, `opencode` in `WRITERS` in `scripts/review-dispatch.py`). OpenCode has no campaign `Stop` hook and no in-session recurring-job tool wired here, so `process-plan` and `process-phase` cannot start a guarded campaign under it, and OpenCode ships sections attended, one at a time, until this section lands. OpenCode V2 server plugins under `.opencode/plugins/` load automatically, can subscribe to the server's `session.idle` events (`ctx.event.subscribe`), queue a prompt into a session (`ctx.session.prompt`), and own timers in `setup` with a cleanup function; the running session's id is `$OPENCODE_SESSION_ID`. This section builds the two missing guard parts on those primitives, through the shared `scripts/campaign_guard.py`, the way `scripts/codex-campaign-hook.py` adapts it for Codex.

**Corrected 2026-10-29 (independent review `docs/reviews/opencode-writer-route.md`, Round 1):** an operator interrupt must never be continued (`process-plan`'s Operator stop; the shared guard skips continuation on `terminationReason: USER_CANCELED`), so the plugin tracks `session.execution.interrupted` and the adapter carries it as that field; and the heartbeat must keep firing for its own cleanup after `end`, a failed cancel, or a handover (the canonical prompt's NOT THE OWNER, NO GUARD and `pending-cancel` steps), so only the hook's continuation is owner-gated, never the heartbeat's delivery.

- [ ] Write `scripts/opencode-campaign-hook.py`, an adapter like `scripts/codex-campaign-hook.py`: it reads one JSON payload (`{"session_id": ..., "cwd": ..., "interrupted": true|false}`) on stdin, bounds its size, maps `"interrupted": true` to `terminationReason: USER_CANCELED`, passes it to `campaign_guard.stop_hook`, prints the guard's decision (or `{}`), and fails open (prints `{}`, exits 0, records the error through the guard) on any malformed payload or thrown error. Done when: `scripts/tests/test_opencode_campaign_hook.py` proves it blocks only the owning session from a subdirectory, lets a non-owner, a missing guard and an interrupted turn pass with no decision, and fails open on an oversized or malformed payload.
- [ ] Write the server plugin `.opencode/plugins/rasterfold-campaign/index.ts`: it records the last execution outcome per session (`session.execution.interrupted` versus `session.execution.succeeded` or `failed`), and on `session.idle` for a session in this checkout runs the adapter with that session's id and the interrupted flag; when the adapter returns a block decision it queues its `reason` into the same session with `ctx.session.prompt`. It also owns the heartbeat: a recurring `3-59/5 * * * *` timer per job, created, listed and cancelled through plugin-registered tools named in the run file, that queues the `process-plan` canonical heartbeat prompt into its session whenever that session is idle, whoever the guard names (the prompt itself decides owner, cleanup or self-deletion through `whoami`), and stops only when the job is cancelled or the session is gone. Every failure fails open (no prompt), and the plugin does nothing outside a RasterFold checkout. Done when: a disposable campaign under OpenCode is driven end to end and its outputs are quoted: `acquire`; an idle turn the plugin blocks with the guard's reason; an interrupted turn the plugin does not continue; a heartbeat firing that `whoami` accepts; `end --reason operator-stop` with the job's cancel deliberately skipped, then a heartbeat firing that runs the `pending-cancel` recovery and deletes its own job; and a second job left behind by an `acquire --handover`, whose next firing reads NOT THE OWNER and deletes itself.
- [ ] Map OpenCode onto the run guard in `.agents/skills/process-plan/SKILL.md` and `.agents/skills/process-phase/SKILL.md` (the session id `$OPENCODE_SESSION_ID`, the plugin's heartbeat tools as `CronCreate`/`CronList`/`CronDelete`, the hook through the plugin), and replace the attended-only rule in `AGENTS.md`. Done when: `python3 scripts/drift-check.py` exits 0 and the skills name the OpenCode mapping.
- [ ] Commit: `"runner: OpenCode campaign Stop hook and heartbeat (D00 T01 §10)"`

**Test checkpoint:** `python3 -m unittest discover -s scripts/tests -v` and `python3 scripts/campaign_guard.py --self-test` exit 0 with `scripts/tests/test_opencode_campaign_hook.py` running, and the driven disposable campaign under OpenCode is quoted (the plugin's block, the uncontinued interrupted turn, the heartbeat's `whoami` line, the `pending-cancel` recovery after a skipped cancel, the handed-over job's NOT THE OWNER self-deletion, and the `end` and `cancel-confirmed` output).

## Verification

- [x] `pwsh scripts/check-all.ps1` -- exits 0 with every gate `PASS`, the static analysis gate included (`PASS clang-tidy (32.1s)`, `check-all: 28/28 passed`)
- [x] `python scripts/todo-graph.py validate` clean (`32 todos, 227 sections -- 0 fatal, 0 warning(s)`)
- [x] Both workflows green on the last push, read back with `python3 scripts/ci_status.py` (`build.yml: green at f4dc09160602 (https://github.com/rizonesoft/RasterFold/actions/runs/40147208946)` and `plan.yml: green at f4dc09160602 (https://github.com/rizonesoft/RasterFold/actions/runs/40147208820)`, exit 0)
