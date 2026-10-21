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
> **Current state (verified 2026-09-30):** The port, independent Codex mechanics and four-target build are committed. This WSL checkout has `core.hooksPath=tools/githooks` and authenticated GitHub CLI; Windows/Linux build and plan workflows have succeeded. Clone provisioning remains required. There is no `.clang-tidy` gate; `COVERAGE_FLOOR` is 31 after the 2026-09-30 audit measured 31/31 covered Current state blocks. Local/remote migration evidence is in `docs/wsl-migration-audit.md`, and formal TODO stamps remain open.
<!-- claim: exists tools/githooks/pre-commit -->
<!-- claim: exists tools/githooks/commit-msg -->
<!-- claim: exists tools/provision.ps1 -->
<!-- claim: exists .conclave/panel.toml -->
<!-- claim: absent .clang-tidy -->
<!-- claim: count "^COVERAGE_FLOOR = 31$" scripts/todo-claims.py = 1 -->

## Inputs

- [`scripts/plan-audit.py`](../../scripts/plan-audit.py) -- prerequisite order and release requirement coverage.

- [`tools/githooks/pre-commit`](../../tools/githooks/pre-commit) -- the hook §1 wires in; it validates the staged tree in a temp checkout
- [`tools/provision.ps1`](../../tools/provision.ps1) -- sets `core.hooksPath` (§1) and verifies the toolchain
- [`.github/workflows/plan.yml`](../../.github/workflows/plan.yml), [`.github/workflows/build.yml`](../../.github/workflows/build.yml) -- the workflows §2 reads back
- [`scripts/check-all.ps1`](../../scripts/check-all.ps1) -- the gate runner §3 adds the static analysis gate to
- [`.conclave/panel.toml`](../../.conclave/panel.toml) and [`scripts/panel_slots.py`](../../scripts/panel_slots.py) -- the review panel §4 proves
- [`.conclave/reviewers.toml`](../../.conclave/reviewers.toml) and [`scripts/review-dispatch.py`](../../scripts/review-dispatch.py) -- the Conclave-SDK reviewer kit's pin and dispatcher, which §6 makes the source of Claude's reviewers
- [`scripts/todo-claims.py`](../../scripts/todo-claims.py) -- `COVERAGE_FLOOR`, which §5 raises
- -> XREF: D99 T01 §2 -- the GitHub CLI login §2 needs before `ci-wait` can read CI back

## Outcome

- `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints mode `100755` for both, and a provisioned clone refuses a commit whose staged tree fails `validate`.
- The first push of `main` has a green `plan-gates` run and a green `build` run, quoted by URL in this file's stamp.
- `scripts/check-all.py` and its PowerShell wrapper run target-aware clang-tidy over Windows/POSIX and x64/ARM64 compilation databases; the gate fails on a warning count above `todo/.tidy-baseline`.
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
|   4   |   §4    | Prove the review panel end to end             | §1, §6     |  [ ]   |
|   5   |   §5    | Raise the claims coverage floor               | §1         |  [ ]   |
|   6   |   §6    | Claude review rounds through the reviewer pin | §1         |  [ ]   |

---

## 1. Commit the Port and Wire the Commit Hook

The hook is the only gate that runs before a commit exists, so a tree that fails `validate` never reaches the remote. It must stay a POSIX shell script with LF endings, because Git for Windows runs hooks under its own `sh`, and it must be recorded executable, because Windows does not keep the file mode.

- [ ] Run `python3 tools/provision.py` in WSL/Linux or `powershell -File tools/provision.ps1` on Windows. Done when: the host provisioner in verification mode exits zero and reports the local toolchain, hook, Python/Pillow and required host checks and `git config core.hooksPath` prints `tools/githooks`. Cheaper substitute: copying the hooks into `.git/hooks`, which no clone inherits.
- [ ] Stage the ported tree (`todo/`, `scripts/`, `.claude/`, `.conclave/`, `tools/githooks/`, `tools/provision.ps1`, `toolchain.json`, `AGENTS.md`, `CLAUDE.md`, `docs/`, `.github/`, `.gitattributes`) and record both hooks executable with `git update-index --chmod=+x tools/githooks/pre-commit tools/githooks/commit-msg`. Done when: `git ls-files -s tools/githooks/pre-commit tools/githooks/commit-msg` prints mode `100755` for both.
- [ ] Confirm `.gitattributes` pins the hooks to LF. Done when: `git check-attr eol -- tools/githooks/pre-commit tools/githooks/commit-msg` prints `eol: lf` for both.
- [ ] Confirm the hook refuses a broken staged tree: stage a copy of a TODO file with its `Commit:` item removed and run `git commit` in a scratch clone. Done when: the commit is refused with `pre-commit: todo-graph validate is red on the staged tree`, and the scratch clone is deleted.
- [ ] Keep `scripts/plan-audit.py` wired into staged hooks, `scripts/check-all.py` and `.github/workflows/plan.yml`, running against the canonical graph and `docs/vision-map.json`. Done when: later prerequisites and disconnected release requirements fail, while all product owners reach the release gate.
- [ ] Commit: `"workspace: port the TODO system, skills, hooks, and gates from Isotone"`

**Test checkpoint:** `pwsh scripts/check-all.ps1` exits 0 with every gate `PASS` at the commit; `git ls-files -s tools/githooks/pre-commit` prints `100755`; the refused scratch commit's message is quoted.

## 2. First Push: CI Green and Read Back

**Needs:** Network (downloads)

A workflow nobody has seen run is a claim, not a gate. The `plan-gates` workflow validates the plan and supported documentation on every main push; the `build` workflow bootstraps the pinned toolchain on a clean runner, builds, and runs the correctness suite. Both are read back with `ci-wait`, so a red run is seen by the session that caused it.

- [ ] Push `main` and read both workflows back with `python scripts/review_prompt.py ci-wait <sha> --since <before>` and `--workflow build --workflow-path .github/workflows/build.yml`. Done when: both print a green conclusion with the run URL. Cheaper substitute: opening the Actions page by hand, which leaves no record.
- [ ] Record both run URLs and their conclusions in this section's stamp. Done when: the stamp quotes both lines as `ci-wait` printed them.
- [ ] Commit: `"workspace: read the first CI runs back"`

**Test checkpoint:** `ci-wait` exits 0 for both workflows on the pushed commit; a deliberately broken TODO pushed to a scratch branch makes `plan-gates` fail, and the red run's URL is quoted before the branch is deleted.

## 3. The Static Analysis Gate

The build already fails on any compiler warning (`-Werror` in `scripts/build.py`). Static analysis finds what the compiler does not: a read past a buffer on one path, an unchecked size, a leak. The gate is a ratchet: the count found on the day it lands is the baseline, and it only shrinks.

- [ ] Add `.clang-tidy` with `bugprone-*`, `clang-analyzer-*`, `performance-*`, and `cert-*` checks, minus the checks that report style rather than defects (`bugprone-easily-swappable-parameters`, `readability-magic-numbers`, and any other noise the first run shows, each named with its reason in a comment). Done when: a `scripts/tidy.py` driver uses each target's compilation database and pinned clang-tidy, covering Windows/POSIX, x64 SIMD and ARM64 portable paths. Cheaper substitute: enabling every check and baselining thousands of style findings.
- [ ] Record the warning count in `todo/.tidy-baseline` as `count: <n>`, counting unique `(file, line, column, check)` findings. Done when: the file exists and its count equals the run's.
- [ ] Add a `clang-tidy` gate to `scripts/check-all.py` (also used by `scripts/check-all.ps1`) that fails when the count exceeds the baseline or any `error:` appears. Done when: the gate passes on the tree, and a probe line added to `src/mem.c` that reads an uninitialized variable makes it fail; the probe is reverted.
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

`COVERAGE_FLOOR` is the number of `Current state` blocks that must carry a claim. It is 31 after the 2026-09-30 measurement of 31/31 covered blocks; remeasure before raising it further.

- [ ] Run `python scripts/todo-claims.py --coverage` and set `COVERAGE_FLOOR` in `scripts/todo-claims.py` to the number of blocks it reports as carrying a claim. Done when: `python scripts/todo-claims.py --coverage` exits 0 and removing one claim from any block makes it exit 1. Cheaper substitute: raising the floor by an arbitrary number.
- [ ] Update this file's claim on `COVERAGE_FLOOR` to the new value. Done when: `python scripts/todo-claims.py` exits 0.
- [ ] Commit: `"workspace: raise the claims coverage floor to the measured count"`

**Test checkpoint:** `python scripts/todo-claims.py --coverage` exits 0 at the new floor and exits 1 with one claim removed (quoted, then restored).

## 6. Claude Review Rounds Through the Reviewer Pin

**Operator decision 2026-10-21:** "Switch stamps to the dispatcher" -- the operator's answer, in an attended session, to: "The reviewer kit adds `.conclave/reviewers.toml` and `review-dispatch.py` (Claude routes to Astra with Gemini as fallback). RasterFold already has `.conclave/panel.toml`, where your recorded decision is Astra only with no fallback slots. How should the two coexist?" This supersedes the no-fallback decision recorded in `.conclave/panel.toml` (Resolute 2026-09-25) for Claude-written work. Codex-written work keeps its private Claude panel (`D00 T04 §3`).

The Conclave-SDK reviewer kit (imported 2026-10-21, `bcbbf2f`) routes Claude's work to GPT-6 Astra with Gemini 4 as the one fallback, but nothing in the stamp path reads it yet: every round runs `scripts/panel_slots.py exec <slot>`, and four places are built around a `GPT panel` record. `stamp-no-opus-panel` (`scripts/todo-validate.py` rule 16) reads only `Opus`, `Claude`, `GPT`, and `Grok` panel headings and fails any other last section; `scripts/todo-runs.py` maps headings to families and accepts only the `openai` and `anthropic` providers; `scripts/review_prompt.py attest` derives a runner identity only for `codex-panel`, `claude-panel`, and `grok-panel`, needing a `model:` banner `agy` does not print; and `review_prompt.py --self-test` pins the skill's slot and no-fallback wording. The dispatcher's own `## Round N:` record is read by nothing else, and `--list-rounds` reports `docs/reviews/todo-vision-audit-2026-09-30.md` unreadable because its `## Round 2 findings:` headings look like dispatcher rounds.

The design keeps each layer's owner. `.conclave/reviewers.toml` owns who reviews Claude's work and the one fallback; `.conclave/panel.toml` keeps the round roles (slot effort and timeout) and stops naming a model for Claude's slots; the validators keep owning the record format. A round still runs as `panel_slots.py exec <slot>` with the fenced prompt on stdin, so the receipt, fences, and attestation keep working; the slot resolves its reviewer from the pin's `claude` route through `review-dispatch.py`'s `load_pin`, the only place the pin's rules live, and fails over to the fallback exactly when the dispatcher would (not installed, non-zero exit, timeout, empty reply, `OVERALL: blocked`). A fallback round is recorded under a `Gemini panel Round N` heading with a `Fallback:` line naming the primary's failure, the same shape the retired Grok fallback used.

- [ ] Make `scripts/panel_slots.py exec <slot>` take Claude's reviewer and fallback from `.conclave/reviewers.toml` through `load_pin` (imported from `scripts/review-dispatch.py`, never re-implemented), run the primary at the slot's effort and timeout, fail over to the fallback on the dispatcher's failure conditions, print `panel_slots: slot S reviewer R model M effort E timeout Ts` (and `panel_slots: fallback R2 after R: <reason>`) on stderr, and run `agy` with the dispatcher's arguments. Done when: `python3 scripts/panel_slots.py --self-test` covers primary success, every failover reason, both reviewers failing (exit 1, no review), and a pin that refuses (exit 2), with fake CLIs. Cheaper substitute: calling `review-dispatch.py` as a subprocess, which drops the stdin fence, the model banner, and the slot's effort.
- [ ] Drop the model from Claude's slots in `.conclave/panel.toml` and teach `panel_slots.py validate` the new shape, keeping the Codex panel (`.codex/runner/panel.toml`) untouched. Done when: `validate` refuses a Claude slot that names a model and accepts the pinned route; `.codex/tests` still pass.
- [ ] Accept `Gemini panel` as the fallback family in `stamp-no-opus-panel` (`scripts/todo-validate.py`), `review-citation-role-mismatch`, the plan-review rung grammar (`gemini rung`), `panel_fallback` and `query plan-health` (`scripts/todo-graph.py`), and in `scripts/todo-runs.py` (`HEADING_FAMILY`, a `google` provider, `family_accepts`). Done when: `python3 scripts/todo-graph.py self-test` and `python3 scripts/todo-runs.py --self-test` include a Gemini fallback stamp that passes with its `Fallback:` line and fails without it, and every existing stamp still validates.
- [ ] Give `scripts/review_prompt.py attest` a `gemini-panel` identity read from the `panel_slots:` stderr line, since `agy` prints no model banner. Done when: `python3 scripts/review_prompt.py --self-test` attests a Gemini fallback round and refuses one whose stderr names no reviewer.
- [ ] Rewrite `.claude/skills/review-todo-section/SKILL.md` (policy line, outage matrix, slot table, findings headings) and `.claude/skills/process-todo-section/SKILL.md` (`independent` slot) for the route and its fallback, and update the `review_prompt.py --self-test` skill pins with them. Done when: no Claude skill says "no fallback", and `python3 scripts/review_prompt.py --self-test` passes on the new wording.
- [ ] Keep ad-hoc `independent-review` rounds out of the stamp record: record them under `docs/reviews/independent/` (move `docs/reviews/conclave-guard-port.md` there and its `historical_links` keys in `docs/drift.json`), and point `.claude/skills/independent-review/SKILL.md` and `.codex/skills/independent-review/SKILL.md` at it. Done when: `python3 scripts/review-dispatch.py --list-rounds --records docs/reviews/independent` lists every round with none unreadable, and `python3 scripts/drift-check.py` exits 0.
- [ ] Update `AGENTS.md` (the `.conclave/` rows and the review rules), `.claude/agent-routing.md`, and the `.conclave/panel.toml` header comment to name the route and its fallback. Done when: `python3 scripts/drift-check.py` exits 0 with the AGENTS.md receipt renewed.
- [ ] Commit: `"review: Claude rounds take their reviewers from the reviewer pin"`

**Test checkpoint:** `python3 scripts/check-all.py` exits 0 (quoted), and one probe round per path is quoted: Astra serving, and Gemini serving after a forced primary failure (a pin naming a missing CLI first, in a scratch copy).

## Verification

- [ ] `pwsh scripts/check-all.ps1` -- exits 0 with every gate `PASS`, the static analysis gate included
- [ ] `python scripts/todo-graph.py validate` clean
- [ ] Both workflows green on the last push, read back with `ci-wait`
