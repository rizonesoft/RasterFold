---
schema_version: 1
id: drift-gates
domain: 08-docs
status: draft
title: "TODO-02 -- Drift and Staleness Gates for Docs, READMEs, and Supporting Files"
depends_on: []
track: D2
---

# TODO-02 -- Drift and Staleness Gates for Docs, READMEs, and Supporting Files

> **Goal:** No document in the repository can quietly disagree with the code or the data. Names, versions, limits, exit codes, and messages come from one facts file; every measured figure in a README or doc is bound to the committed data it came from; generated files are checked against their sources; every page states when it was verified against the files it describes and goes stale when they change; and every link resolves. The commit hook, `scripts/check-all.ps1`, and CI all refuse drift.

> [!IMPORTANT]
> **Current state (verified 2026-09-30):** `scripts/drift-check.py` and `docs/drift.json` are implemented by the attended 2026-09-30 maintenance task, together with staged hooks, local gates and CI. They cover all supported tracked/new information files, explicit source facts, data bindings, generated blocks, source review hashes, references, built-site receipts and deterministic repair. Historical measurements retain their dates and limitations; absent future data and a not-yet-built Pages site cannot be called current proof. Formal section stamps remain separate from this implementation.
<!-- claim: exists scripts/drift-check.py -->
<!-- claim: exists docs/drift.json -->

## Inputs

- [`scripts/todo-claims.py`](../../scripts/todo-claims.py) -- the claim checker §1 extends to the whole repository
- [`README.md`](../../README.md), [`docs/research.md`](../../docs/research.md), [`research/README.md`](../../research/README.md), [`spec/FOLD.md`](../../spec/FOLD.md), [`AGENTS.md`](../../AGENTS.md) -- the documents under the gates
- [`tools/githooks/pre-commit`](../../tools/githooks/pre-commit), [`scripts/check-all.ps1`](../../scripts/check-all.ps1), [`.github/workflows/plan.yml`](../../.github/workflows/plan.yml) -- where §5 wires the gates
- -> XREF: D00 T02 §3 -- `benchmarks/corpus.csv`, the corpus data §2 binds figures to
- -> XREF: D08 T01 §1 -- the site generator whose `--check` §3 runs

## Outcome

- `python scripts/drift-check.py` exits 0 on the tree and 1 on each seeded kind of drift (a wrong fact, a figure unlike its data, a stale generated file, a page whose reviewed source hashes differ, a dead link).
- The commit hook, `scripts/check-all.ps1`, and the `plan-gates` workflow run it.

**Adjacency:** list=not-applicable (a gate has no records to browse); document=applicable @ D08 T02 §6; settings=not-applicable (no option); reporting=applicable @ D08 T02 §4; notifications=not-applicable (a gate reports on the console and in CI); permissions=applicable @ D08 T02 §5; audit=applicable @ D08 T02 §7; exchange=not-applicable (nothing imported or exported); reverse=not-applicable (repair uses normal Git diffs; it never rewrites history)

**Adjacency rationale:** Reporting is the staleness report; permissions is the policy that refuses a drifting commit.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | One facts file and claims everywhere | -- |  [ ]   |
|   2   |   §2    | Figures bound to their data | §1, D00 T02 §3 |  [ ]   |
|   3   |   §3    | Generated files match their sources | §1, D08 T01 §1 |  [ ]   |
|   4   |   §4    | Staleness: verified dates and data revisions | §2 |  [ ]   |
|   5   |   §5    | Links, and the gates wired everywhere | §3, §4 |  [ ]   |
| 6 | §6 | Deterministic repair and complete file coverage | §1, §3 | [ ] |
| 7 | §7 | Commit and built-site provenance | §5, §6 | [ ] |

---

## 1. One Facts File and Claims Everywhere

- [ ] Write `docs/facts.json`: the product and format names, the codec and format versions, the size limits, the exit codes and their meanings, the command names, and the file extensions. Done when: every value matches `src/rf.h` and `src/main.c`, checked by `scripts/drift-check.py`. Cheaper substitute: a list in the README.
- [ ] Implement explicit fact bindings, generated current-fact blocks and supported prose assertions in `scripts/drift-check.py`. Done when: a wrong current format/API/limit value and a malformed binding fail; unbound arbitrary prose is reported as outside semantic proof, never silently repaired.
- [ ] Reuse `scripts/todo-claims.py` claim evaluation from `scripts/drift-check.py` across supported information files. Done when: a stale or unterminated claim in `README.md` fails, while fenced examples and historical/planned roles remain explicit.
- [ ] Commit: `"docs: one facts file and claims checked in every document"`

**Test checkpoint:** `python scripts/drift-check.py` exits 0; the two seeded drifts above each exit 1 (quoted, then reverted).

## 2. Figures Bound to Their Data

- [ ] Add a `<!-- drift:fact <provider-key> = <value> -->` comment form, placed beside each measured figure in a README or doc, whose selector names a provider in `docs/drift.json` with an exact CSV row selector and column (an aggregate needs an explicitly tested provider), and make `scripts/drift-check.py` recompute each one and compare. Done when: every current published measurement carries a binding and provenance; legacy research is labeled historical rather than falsely bound to newer data. Cheaper substitute: rounding the figures vaguely so they never look wrong.
- [ ] Require current measurement blocks to be generated from registered providers, keeping historical reports outside current-performance claims. Done when: editing a current generated measurement block makes the check exit 1.
- [ ] Commit: `"docs: bind every measured figure to its data"`

**Test checkpoint:** changing one CSV cell makes the check name the figure that no longer matches; `python scripts/drift-check.py` exits 0 on the tree.

## 3. Generated Files Match Their Sources

- [ ] Run every generator's `--check` from `scripts/drift-check.py`: `python scripts/build-docs.py --check`, `python research/milestones.py --check`, `python scripts/todo-graph.py plan --check`, and (once they exist) the command-line and API references. Done when: a stale output of each makes the check exit 1. Cheaper substitute: regenerating in CI without failing.
- [ ] Commit: `"docs: every generated file checked against its source"`

**Test checkpoint:** each seeded stale output quoted with its failure line.

## 4. Staleness: Verified Dates and Data Revisions

**Job:** A maintainer can read a report of every stale page and data file, and why it is stale.

- [ ] Register source hashes and review evidence in `docs/drift.json` for current prose. Done when: a same-day source change fails the receipt and `--fix` cannot renew it; explicit review is required.
- [ ] Have the measurement producers record actual codec revision and `KAT_FOLD` hash alongside new `benchmarks/results.csv` and `benchmarks/corpus.csv` data, and fail when the bitstream changed after the data was produced (the known-answer hash differs), naming the data files to regenerate. Done when: regenerating `KAT_FOLD` without rerunning the benchmarks makes the check exit 1.
- [ ] Commit: `"docs: staleness from verified dates and data revisions"`

**Test checkpoint:** the two seeded staleness cases quoted; the report names each stale page and changed source; no date-only freshness inference is accepted.

## 5. Links, and the Gates Wired Everywhere

**Job:** A contributor meets the policy at every step: the commit hook, the local gates, and CI refuse drift the same way.

- [ ] Check every relative link and heading anchor in tracked Markdown, and in the allowlisted Jekyll artifact in CI (`build/site-artifact/`). Done when: a dead link makes the check exit 1; extend source-anchor fixtures to cover Setext headings before such headings are admitted, while built HTML IDs remain authoritative.
- [ ] Run `python scripts/drift-check.py` in `tools/githooks/pre-commit` (on the staged tree), in `scripts/check-all.ps1`, and in `.github/workflows/plan.yml`. Done when: each of the three refuses a seeded drift.
- [ ] Commit: `"docs: refuse drift in the hook, the local gates, and CI"`

**Test checkpoint:** a scratch-clone commit with a seeded drift refused by the hook (quoted); `pwsh scripts/check-all.ps1` shows the `drift` gate `PASS` on the tree.

## 6. Deterministic repair and complete file coverage

**Job:** A maintainer can render source facts into generated documents and repair stale output while preserving authored and historical evidence.

- [ ] Inventory supported Markdown, text, HTML, structured configuration, data and SVG/XML in `docs/drift.json`. Done when: all tracked and newly added supported files have a visible role and applicable checks, with explicit legacy encodings.
- [ ] Implement compare-before-write atomic repair in `scripts/drift-check.py --fix`. Done when: only registered generated files/blocks and generator outputs change; concurrent edits and paths outside the checkout are refused.
- [ ] Keep review receipts, measurements and commit history out of automatic repair. Done when: a stale receipt or unresolved semantic mismatch remains a failing report after safe repairs.
- [ ] Add `tests/test_drift.py` fixtures. Done when: wrong facts, missing coverage, malformed contracts, historical preservation, symlink escapes and idempotent repair are exercised.
- [ ] Commit: `"docs: deterministic repair and complete file coverage"`

**Test checkpoint:** Run `python3 -m unittest discover -s tests -p test_drift.py -v`; quote wrong-fact failure, narrow repair diff and a second no-op repair.

## 7. Commit and built-site provenance

**Job:** A maintainer can audit commit history and prove documentation and the published site correspond to their cited revisions and current inputs.

- [ ] Validate explicit commit/path references and declared commit facts in `scripts/drift-check.py`. Done when: an unknown revision/path fails; facts in old commits are evaluated against that commit, never rewritten to match HEAD.
- [ ] Generate and verify `build/site-artifact/drift-build.json` from allowlisted source hashes. Done when: a changed source, missing artifact or broken HTML fragment blocks publishing.
- [ ] Wire the staged document gate and commit-message declaration checks into `tools/githooks/`. Done when: unstaged fixes cannot hide a bad staged document or a false staged commit fact.
- [ ] Add exact-candidate CI checks to `.github/workflows/plan.yml`. Done when: every supported information-file change runs drift checks, including READMEs and Pages sources.
- [ ] Commit: `"docs: commit and built-site provenance"`

**Test checkpoint:** Seed a bad staged document, bad commit reference and stale/broken built site in fixtures; each fails. Verify a correct historical commit fact against its own tree.

**Groomed 2026-09-30:** Mechanical coverage is explicit, not a claim that arbitrary prose can be understood perfectly. Future measurement producers remain prerequisites of complete live measurement coverage. Repair never fabricates a benchmark, a review stamp or a deployment.

## Verification

- [ ] `python scripts/drift-check.py` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
