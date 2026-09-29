---
schema_version: 1
id: dev-automation
domain: 00-workspace
status: draft
title: "TODO-01 -- Dev-Automation Wiring"
depends_on: []
track: W1
---

# TODO-01 -- Dev-Automation Wiring

> **Goal:** The plan polices itself on every clone and every push: the ported TODO system, skills, hooks, and gates are committed with the hooks executable, the commit hook refuses a broken staged tree, the first push comes back green from both workflows, a static analysis gate holds the C sources to a baseline that only shrinks, the review panel is proven end to end before the first stamp depends on it, and the claims ratchet sits at what the tree measures.

> [!IMPORTANT]
> **Current state (verified 2026-09-29):** The TODO tooling (`scripts/todo-graph.py` and its siblings), the skills under `.claude/skills/`, the Stop hook, `.conclave/panel.toml`, `scripts/check-all.ps1`, `tools/provision.ps1`, `toolchain.json`, and the hooks under `tools/githooks/` were ported from Isotone on 2026-09-29 and are not committed yet. `core.hooksPath` is not set in this clone until `tools/provision.ps1` runs. There is no `.clang-tidy` and no static analysis gate. The GitHub CLI (`gh`), which `python scripts/review_prompt.py ci-wait` needs to read CI back, is not installed on the development machine. `COVERAGE_FLOOR` in `scripts/todo-claims.py` is 1.
<!-- claim: exists tools/githooks/pre-commit -->
<!-- claim: exists tools/githooks/commit-msg -->
<!-- claim: exists tools/provision.ps1 -->
<!-- claim: exists .conclave/panel.toml -->
<!-- claim: absent .clang-tidy -->
<!-- claim: count "^COVERAGE_FLOOR = 1$" scripts/todo-claims.py = 1 -->

## Inputs

- [`tools/githooks/pre-commit`](../../tools/githooks/pre-commit) -- the hook §1 wires in; it validates the staged tree in a temp checkout
- [`tools/provision.ps1`](../../tools/provision.ps1) -- sets `core.hooksPath` (§1) and verifies the toolchain
- [`.github/workflows/plan.yml`](../../.github/workflows/plan.yml), [`.github/workflows/build.yml`](../../.github/workflows/build.yml) -- the workflows §2 reads back
- [`scripts/check-all.ps1`](../../scripts/check-all.ps1) -- the gate runner §3 adds the static analysis gate to
- [`.conclave/panel.toml`](../../.conclave/panel.toml) and [`scripts/panel_slots.py`](../../scripts/panel_slots.py) -- the review panel §4 proves
- [`scripts/todo-claims.py`](../../scripts/todo-claims.py) -- `COVERAGE_FLOOR`, which §5 raises
- -> XREF: D99 T01 §2 -- the GitHub CLI login §2 needs before `ci-wait` can read CI back

## Outcome

- `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints mode `100755` for both, and a provisioned clone refuses a commit whose staged tree fails `validate`.
- The first push of `main` has a green `plan-gates` run and a green `build` run, quoted by URL in this file's stamp.
- `pwsh scripts/check-all.ps1` runs a `clang-tidy` gate over `src/*.c` that fails on a warning count above `todo/.tidy-baseline`.
- One review slot of each kind has run against a probe and its record is committed, so the first real stamp does not discover a broken panel.
- `COVERAGE_FLOOR` equals the number of `Current state` blocks carrying a claim on the day §5 ships.

**Adjacency:** all=not-applicable (repository plumbing: gates, hooks, and CI with no user-facing file, option, or output)

**Adjacency rationale:** Everything in this file is developer tooling. It changes no byte the codec writes and no option a user passes.

## Implementation Order

| Order | Section | Deliverable                                   | Depends On | Status |
| :---: | :-----: | --------------------------------------------- | ---------- | :----: |
|   1   |   §1    | Commit the port and wire the commit hook      | --         |  [ ]   |
|   2   |   §2    | First push: CI green and read back            | §1, D99 T01 §2 |  [ ]   |
|   3   |   §3    | The static analysis gate                      | §1         |  [ ]   |
|   4   |   §4    | Prove the review panel end to end             | §1         |  [ ]   |
|   5   |   §5    | Raise the claims coverage floor               | §1         |  [ ]   |

---

## 1. Commit the Port and Wire the Commit Hook

The hook is the only gate that runs before a commit exists, so a tree that fails `validate` never reaches the remote. It must stay a POSIX shell script with LF endings, because Git for Windows runs hooks under its own `sh`, and it must be recorded executable, because Windows does not keep the file mode.

- [ ] Run `pwsh tools/provision.ps1`. Done when: `pwsh tools/provision.ps1 -Verify` prints `provision: 8/8 required legs green` and `git config core.hooksPath` prints `tools/githooks`. Cheaper substitute: copying the hooks into `.git/hooks`, which no clone inherits.
- [ ] Stage the ported tree (`todo/`, `scripts/`, `.claude/`, `.conclave/`, `tools/githooks/`, `tools/provision.ps1`, `toolchain.json`, `AGENTS.md`, `CLAUDE.md`, `docs/`, `.github/`, `.gitattributes`) and record both hooks executable with `git update-index --chmod=+x tools/githooks/pre-commit tools/githooks/commit-msg`. Done when: `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints mode `100755` for both.
- [ ] Confirm `.gitattributes` pins the hooks to LF. Done when: `git check-attr eol -- tools/githooks/pre-commit tools/githooks/commit-msg` prints `eol: lf` for both.
- [ ] Confirm the hook refuses a broken staged tree: stage a copy of a TODO file with its `Commit:` item removed and run `git commit` in a scratch clone. Done when: the commit is refused with `pre-commit: todo-graph validate is red on the staged tree`, and the scratch clone is deleted.
- [ ] Commit: `"workspace: port the TODO system, skills, hooks, and gates from Isotone"`

**Test checkpoint:** `pwsh scripts/check-all.ps1` exits 0 with every gate `PASS` at the commit; `git ls-files -s tools/githooks/pre-commit` prints `100755`; the refused scratch commit's message is quoted.

## 2. First Push: CI Green and Read Back

**Needs:** Network (downloads)

A workflow nobody has seen run is a claim, not a gate. The `plan-gates` workflow validates the plan on every push that touches it; the `build` workflow bootstraps the pinned toolchain on a clean runner, builds, and runs the correctness suite. Both are read back with `ci-wait`, so a red run is seen by the session that caused it.

- [ ] Push `main` and read both workflows back with `python scripts/review_prompt.py ci-wait <sha> --since <before>` and `--workflow build --workflow-path .github/workflows/build.yml`. Done when: both print a green conclusion with the run URL. Cheaper substitute: opening the Actions page by hand, which leaves no record.
- [ ] Record both run URLs and their conclusions in this section's stamp. Done when: the stamp quotes both lines as `ci-wait` printed them.
- [ ] Commit: `"workspace: read the first CI runs back"`

**Test checkpoint:** `ci-wait` exits 0 for both workflows on the pushed commit; a deliberately broken TODO pushed to a scratch branch makes `plan-gates` fail, and the red run's URL is quoted before the branch is deleted.

## 3. The Static Analysis Gate

The build already fails on any compiler warning (`-Werror` in `build.ninja`). Static analysis finds what the compiler does not: a read past a buffer on one path, an unchecked size, a leak. The gate is a ratchet: the count found on the day it lands is the baseline, and it only shrinks.

- [ ] Add `.clang-tidy` with `bugprone-*`, `clang-analyzer-*`, `performance-*`, and `cert-*` checks, minus the checks that report style rather than defects (`bugprone-easily-swappable-parameters`, `readability-magic-numbers`, and any other noise the first run shows, each named with its reason in a comment). Done when: `tools/llvm-mingw/bin/clang-tidy.exe --quiet src/*.c -- -std=c11 -march=x86-64-v2 -Isrc` runs to completion. Cheaper substitute: enabling every check and baselining thousands of style findings.
- [ ] Record the warning count in `todo/.tidy-baseline` as `count: <n>`, counting unique `(file, line, column, check)` findings. Done when: the file exists and its count equals the run's.
- [ ] Add a `clang-tidy` gate to `scripts/check-all.ps1` that fails when the count exceeds the baseline or any `error:` appears. Done when: the gate passes on the tree, and a probe line added to `src/mem.c` that reads an uninitialized variable makes it fail; the probe is reverted.
- [ ] Commit: `"workspace: gate the C sources on clang-tidy against a shrinking baseline"`

**Test checkpoint:** `pwsh scripts/check-all.ps1` shows the `clang-tidy` gate `PASS`; the probe run's `FAIL` line and its finding are quoted.

## 4. Prove the Review Panel End to End

The first real stamp should not be the first time the panel runs. Every slot in `.conclave/panel.toml` runs once against a small probe diff, so a missing CLI, an expired login, or a renamed model shows up here instead of in the middle of a review.

- [ ] Run `python scripts/panel_slots.py exec <slot>` for `bulk`, `signoff`, `depth`, `plan-primary`, `stamp-check`, and `arch-primary` with a fenced probe prompt (`python scripts/review_prompt.py fence PROBE ...`), and `exec independent --commit <sha>` on the §1 commit. Done when: each prints its pin line and returns output that `check-panel`, `check-plan`, or `check-stamp` accepts as the shape its slot owes. Cheaper substitute: `codex --version`, which proves the CLI exists and nothing else.
- [ ] Record each probe in `docs/reviews/run-records.md` in the shape `scripts/todo-runs.py` documents. Done when: `python scripts/todo-runs.py --check` exits 0 with the probe runs listed.
- [ ] Update each model's `probed` date in `.conclave/panel.toml`. Done when: `python scripts/panel_slots.py validate` exits 0 and every probed date is the day of the run.
- [ ] Commit: `"workspace: prove every review slot against a probe"`

**Test checkpoint:** `python scripts/panel_slots.py validate` and `python scripts/todo-runs.py --check` exit 0; each slot's first stderr line (the pin) is quoted.

## 5. Raise the Claims Coverage Floor

`COVERAGE_FLOOR` is the number of `Current state` blocks that must carry a claim. It sits at 1 from the port; the tree authored on 2026-09-29 carries more.

- [ ] Run `python scripts/todo-claims.py --coverage` and set `COVERAGE_FLOOR` in `scripts/todo-claims.py` to the number of blocks it reports as carrying a claim. Done when: `python scripts/todo-claims.py --coverage` exits 0 and removing one claim from any block makes it exit 1. Cheaper substitute: raising the floor by an arbitrary number.
- [ ] Update this file's claim on `COVERAGE_FLOOR` to the new value. Done when: `python scripts/todo-claims.py` exits 0.
- [ ] Commit: `"workspace: raise the claims coverage floor to the measured count"`

**Test checkpoint:** `python scripts/todo-claims.py --coverage` exits 0 at the new floor and exits 1 with one claim removed (quoted, then restored).

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0 with every gate `PASS`, the static analysis gate included
- [ ] `python scripts/todo-graph.py validate` clean
- [ ] Both workflows green on the last push, read back with `ci-wait`
