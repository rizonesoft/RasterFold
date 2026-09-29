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
> **Current state (verified 2026-09-29):** Figures in `README.md` and `docs/research.md` are typed by hand (for example the benchmark table and the 0.971 corpus ratio); nothing checks them against `benchmarks/results.csv`. Claim comments are re-measured only under `todo/`. Links are unchecked. The one generated-file check is `plan --check` for `todo/implementation-plan.md`, plus `python research/milestones.py --check`.
<!-- claim: absent docs/facts.json -->
<!-- claim: absent scripts/drift-check.py -->

## Inputs

- [`scripts/todo-claims.py`](../../scripts/todo-claims.py) -- the claim checker §1 extends to the whole repository
- [`README.md`](../../README.md), [`docs/research.md`](../../docs/research.md), [`research/README.md`](../../research/README.md), [`spec/FOLD.md`](../../spec/FOLD.md), [`AGENTS.md`](../../AGENTS.md) -- the documents under the gates
- [`tools/githooks/pre-commit`](../../tools/githooks/pre-commit), [`scripts/check-all.ps1`](../../scripts/check-all.ps1), [`.github/workflows/plan.yml`](../../.github/workflows/plan.yml) -- where §5 wires the gates
- -> XREF: D00 T02 §3 -- `benchmarks/corpus.csv`, the corpus data §2 binds figures to
- -> XREF: D08 T01 §1 -- the site generator whose `--check` §3 runs

## Outcome

- `python scripts/drift-check.py` exits 0 on the tree and 1 on each seeded kind of drift (a wrong fact, a figure unlike its data, a stale generated file, a page verified before a file it describes changed, a dead link).
- The commit hook, `scripts/check-all.ps1`, and the `plan-gates` workflow run it.

**Adjacency:** list=not-applicable (a gate has no records to browse); document=not-applicable (it checks documents, it writes none); settings=not-applicable (no option); reporting=applicable @ D08 T02 §4; notifications=not-applicable (a gate reports on the console and in CI); permissions=applicable @ D08 T02 §5; audit=not-applicable (git history is the audit); exchange=not-applicable (nothing imported or exported); reverse=not-applicable (a gate changes nothing)

**Adjacency rationale:** Reporting is the staleness report; permissions is the policy that refuses a drifting commit.

## Implementation Order

| Order | Section | Deliverable | Depends On | Status |
| :---: | :-----: | ----------- | ---------- | :----: |
|   1   |   §1    | One facts file and claims everywhere | -- |  [ ]   |
|   2   |   §2    | Figures bound to their data | §1, D00 T02 §3 |  [ ]   |
|   3   |   §3    | Generated files match their sources | §1 |  [ ]   |
|   4   |   §4    | Staleness: verified dates and data revisions | §2 |  [ ]   |
|   5   |   §5    | Links, and the gates wired everywhere | §3, §4 |  [ ]   |

---

## 1. One Facts File and Claims Everywhere

- [ ] Write `docs/facts.json`: the product and format names, the codec and format versions, the size limits, the exit codes and their meanings, the command names, and the file extensions. Done when: every value matches `src/rf.h` and `src/main.c`, checked by `scripts/drift-check.py`. Cheaper substitute: a list in the README.
- [ ] Make `scripts/drift-check.py` fail when a README, doc, or help text states one of those facts differently (the limits, exit codes, versions, command names). Done when: changing `32 megapixels` to `64 megapixels` in `README.md` makes it exit 1.
- [ ] Extend `scripts/todo-claims.py` to re-measure claim comments in every tracked Markdown file, not only under `todo/`. Done when: a stale claim in `README.md` makes it exit 1.
- [ ] Commit: `"docs: one facts file and claims checked in every document"`

**Test checkpoint:** `python scripts/drift-check.py` exits 0; the two seeded drifts above each exit 1 (quoted, then reverted).

## 2. Figures Bound to Their Data

- [ ] Add a `<!-- fact: <file>:<selector> = <value> -->` comment form, placed beside each measured figure in a README or doc, whose selector names a CSV cell or aggregate (for example `benchmarks/results.csv:photos/rasterfold/size_geomean`), and make `scripts/drift-check.py` recompute each one and compare. Done when: every measured figure in `README.md` and `docs/research.md` section 1 carries its binding and the check passes. Cheaper substitute: rounding the figures vaguely so they never look wrong.
- [ ] Refuse a measured-looking figure in those sections that carries no binding (a percentage, a ratio, a byte count, a time, or a size in a table row). Done when: adding an unbound figure makes the check exit 1.
- [ ] Commit: `"docs: bind every measured figure to its data"`

**Test checkpoint:** changing one CSV cell makes the check name the figure that no longer matches; `python scripts/drift-check.py` exits 0 on the tree.

## 3. Generated Files Match Their Sources

- [ ] Run every generator's `--check` from `scripts/drift-check.py`: `python scripts/build-docs.py --check`, `python research/milestones.py --check`, `python scripts/todo-graph.py plan --check`, and (once they exist) the command-line and API references. Done when: a stale output of each makes the check exit 1. Cheaper substitute: regenerating in CI without failing.
- [ ] Commit: `"docs: every generated file checked against its source"`

**Test checkpoint:** each seeded stale output quoted with its failure line.

## 4. Staleness: Verified Dates and Data Revisions

**Job:** A maintainer can read a report of every stale page and data file, and why it is stale.

- [ ] Give every doc page and README a `Verified: YYYY-MM-DD against <paths>` line, and make `scripts/drift-check.py` report a page whose listed paths changed in git after that date. New staleness fails; existing staleness is a ratchet in `docs/.stale-baseline` that only shrinks. Done when: editing `src/main.c` after the README's verified date makes the README stale.
- [ ] Record the codec revision and the `KAT_FOLD` hash in the header of `benchmarks/results.csv` and `benchmarks/corpus.csv`, and fail when the bitstream changed after the data was produced (the known-answer hash differs), naming the data files to regenerate. Done when: regenerating `KAT_FOLD` without rerunning the benchmarks makes the check exit 1.
- [ ] Commit: `"docs: staleness from verified dates and data revisions"`

**Test checkpoint:** the two seeded staleness cases quoted; the report lists each stale page with the commit that made it stale.

## 5. Links, and the Gates Wired Everywhere

**Job:** A contributor meets the policy at every step: the commit hook, the local gates, and CI refuse drift the same way.

- [ ] Check every relative link and heading anchor in tracked Markdown and in `docs/site/`. Done when: a dead link makes the check exit 1.
- [ ] Run `python scripts/drift-check.py` in `tools/githooks/pre-commit` (on the staged tree), in `scripts/check-all.ps1`, and in `.github/workflows/plan.yml`. Done when: each of the three refuses a seeded drift.
- [ ] Commit: `"docs: refuse drift in the hook, the local gates, and CI"`

**Test checkpoint:** a scratch-clone commit with a seeded drift refused by the hook (quoted); `pwsh scripts/check-all.ps1` shows the `drift` gate `PASS` on the tree.

## Verification

- [ ] `python scripts/drift-check.py` exits 0
- [ ] `python scripts/todo-graph.py validate` clean
